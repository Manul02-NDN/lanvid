#pragma once
// l2_protocol.hpp — LanVid Grid Layer 2 Ethernet Audio & Control Protocol (IEEE 802.3 / 802.1Q / 802.1p).
// Direct MAC-to-MAC framing (EtherType 0x88B6) bypassing OS TCP/IP stacks for sub-millisecond (< 1 ms)
// deterministic transport of up to 256 channels (24-bit / 48-96 kHz), L2 Discovery, Clock Master Sync,
// Decentralized Peer-to-Peer Patchbay Rules, and Distributed DSP Offloading + Failover Redundancy.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lv::grid {

constexpr uint16_t kGridEtherType = 0x88B6;   // IEEE 802 Local Experimental EtherType 2
constexpr uint16_t kVlanEtherType = 0x8100;   // IEEE 802.1Q VLAN tag
constexpr uint8_t kGridProtoVersion = 1;
constexpr uint16_t kGridMaxChannels = 256;    // Full 256-channel studio/concert capacity
constexpr size_t kEthHeaderSize = 14;         // Dst(6) + Src(6) + EtherType(2)
constexpr size_t kVlanTagSize = 4;            // TPID(2) + TCI(2)
constexpr size_t kL2HeaderSize = 16;          // Compact LanVid Grid header
constexpr size_t kMaxEthFrameSize = 1522;     // Standard 1500 MTU + 18B Eth+VLAN + 4B FCS margin

struct MacAddress {
    std::array<uint8_t, 6> bytes{{0, 0, 0, 0, 0, 0}};

    bool operator==(const MacAddress& o) const { return bytes == o.bytes; }
    bool operator!=(const MacAddress& o) const { return bytes != o.bytes; }
    bool operator<(const MacAddress& o) const { return bytes < o.bytes; }

    bool is_zero() const {
        return (bytes[0] | bytes[1] | bytes[2] | bytes[3] | bytes[4] | bytes[5]) == 0;
    }
    bool is_broadcast() const {
        return bytes[0] == 0xFF && bytes[1] == 0xFF && bytes[2] == 0xFF &&
               bytes[3] == 0xFF && bytes[4] == 0xFF && bytes[5] == 0xFF;
    }

    static MacAddress broadcast() { return MacAddress{{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}}; }
    static bool parse(const std::string& str, MacAddress& out);
    std::string to_string() const;
};

enum class L2PacketType : uint8_t {
    kAudio = 0x01,         // PCM24 / Float32 multi-channel audio slice (16/32/48 samples)
    kSyncClock = 0x02,     // Clock Master deterministic sync pulse (1 ms / 48 samples)
    kDiscoveryReq = 0x03,  // Broadcast L2 node discovery probe
    kDiscoveryResp = 0x04, // Node capability announcement (MAC, role, channels, CPU)
    kRouteCtrl = 0x05,     // Decentralized P2P route instruction (Node A -> MAC B, ch range)
    kDspParam = 0x06,      // Real-time DSP rack plugin parameter update
    kHeartbeat = 0x07,     // DSP server telemetry (CPU %, buffer %, Primary/Backup state)
};

enum class L2AudioFormat : uint8_t {
    kPcm24Packed = 0,  // 3 bytes/sample (256ch @ 48 kHz = ~295 Mbps raw payload)
    kFloat32 = 1,      // 4 bytes/sample IEEE float32
};

enum class GridNodeRole : uint8_t {
    kStudioHost = 0,   // Control hub + optional audio I/O
    kDawDriver = 1,    // Virtual 256ch ASIO / CoreAudio driver endpoint
    kStageboxIo = 2,   // Hardware or software I/O stagebox
    kDspServer = 3,    // Dedicated Linux RT / cross-platform DSP offload server
};

struct VlanQosTag {
    bool enabled = false;
    uint16_t vlan_id = 0;   // 0..4094 (0 = priority-tagged only, IEEE 802.1p)
    uint8_t pcp = 6;        // 0..7 (6 = Voice / Ultra-Low Latency Audio, highest user priority)
};

// Compact 16-byte LanVid Grid L2 header immediately following EtherType 0x88B6.
struct LvL2Header {
    uint8_t version = kGridProtoVersion;
    L2PacketType type = L2PacketType::kAudio;
    uint16_t stream_id = 0;        // Stream / Rack ID
    uint16_t seq = 0;              // Monotonic sequence per stream
    uint16_t channel_start = 0;    // 0..255 first channel index in this frame
    uint8_t channel_count = 0;     // 1..255 channels packed in this frame
    uint8_t samples_per_ch = 0;    // 16 / 32 / 48 samples per channel
    L2AudioFormat audio_fmt = L2AudioFormat::kPcm24Packed;
    uint8_t flags = 0;             // bit0 = ClockMaster, bit1 = DspReturn, bit2 = BackupServer
    uint32_t media_ts_lo = 0;      // Lower 32 bits of 48/96 kHz media sample clock
};

struct ParsedL2Frame {
    MacAddress dst{};
    MacAddress src{};
    VlanQosTag vlan{};
    LvL2Header hdr{};
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
};

// ---- Control Payloads ----

struct L2SyncClockPayload {
    uint64_t master_sample_pos = 0;   // Exact 64-bit sample counter on Clock Master
    uint64_t master_wallclock_us = 0; // Monotonic microseconds on Clock Master
    uint32_t sample_rate = 48000;     // 48000 / 96000
    uint32_t clock_epoch = 1;         // Increments if Clock Master changes
    uint8_t clock_priority = 128;     // Lower = higher priority
    uint8_t reserved[3] = {};
};
constexpr size_t kL2SyncClockSize = 28;

struct L2DiscoveryPayload {
    GridNodeRole role = GridNodeRole::kStudioHost;
    uint8_t flags = 0;                // bit0=clock_master, bit1=backup_dsp, bit2=rt_kernel
    uint16_t max_channels = kGridMaxChannels;
    uint32_t sample_rate = 48000;
    uint16_t cpu_load_pct_x10 = 0;    // 0..1000 (0.0%..100.0%)
    uint16_t buffer_fill_pct_x10 = 0;
    uint32_t proto_caps = 0;
    char node_name[32] = {};
};
constexpr size_t kL2DiscoverySize = 48;

struct L2RouteRulePayload {
    uint8_t op = 1;                   // 0 = clear all, 1 = add/update route, 2 = remove route
    uint8_t flags = 0;                // bit0 = mute, bit1 = backup_mirror
    uint16_t route_id = 0;
    MacAddress dst_mac{};             // Target destination MAC (direct P2P!)
    uint16_t src_ch_start = 0;        // 0..255
    uint16_t dst_ch_start = 0;        // 0..255
    uint16_t channel_count = 0;       // 1..256
    uint16_t stream_id = 0;
    int16_t gain_cb = 0;              // 0.01 dB (-6000..+1200)
    uint16_t reserved = 0;
};
constexpr size_t kL2RouteRuleSize = 24;

struct L2DspParamPayload {
    uint16_t channel = 0;             // 0..255 (0xFFFF = all channels)
    uint8_t slot_index = 0;           // 0..7 plugin rack slot
    uint8_t flags = 0;                // bit0=bypass, bit1=hpf_on, bit2=eq_on, bit3=comp_on, bit4=gate_on
    float hpf_hz = 80.0f;
    float eq_low_db = 0.0f;
    float eq_mid_hz = 1200.0f;
    float eq_mid_db = 0.0f;
    float eq_high_db = 0.0f;
    float comp_thresh_db = -18.0f;
    float comp_ratio = 3.0f;
    float comp_makeup_db = 0.0f;
    float gate_thresh_db = -60.0f;
    float output_gain_db = 0.0f;
    uint32_t revision = 0;
};
constexpr size_t kL2DspParamSize = 48;

struct L2HeartbeatPayload {
    uint32_t uptime_ms = 0;
    uint16_t cpu_load_pct_x10 = 0;
    uint16_t buffer_fill_pct_x10 = 0;
    uint16_t active_channels = 0;
    uint8_t is_backup = 0;
    uint8_t health_ok = 1;
    uint32_t frames_processed = 0;
};
constexpr size_t kL2HeartbeatSize = 16;

// ---- Frame & Payload Serialization ----

size_t write_l2_frame(uint8_t* out, size_t cap, const MacAddress& dst, const MacAddress& src,
                      const VlanQosTag& vlan, const LvL2Header& hdr,
                      const uint8_t* payload, size_t payload_len);

bool parse_l2_frame(const uint8_t* frame, size_t len, ParsedL2Frame& out);

// Audio sample packing (channel-interleaved planar blocks -> compact 24-bit or Float32)
size_t l2_audio_payload_bytes(L2AudioFormat fmt, uint16_t channels, uint16_t samples_per_ch);
size_t pack_l2_audio_planar(L2AudioFormat fmt, const float* const* planes, uint16_t ch_start,
                            uint16_t ch_count, uint16_t samples_per_ch, uint8_t* out, size_t cap);
bool unpack_l2_audio_planar(L2AudioFormat fmt, const uint8_t* in, size_t in_len,
                            uint16_t ch_count, uint16_t samples_per_ch, float* const* out_planes,
                            uint16_t dst_ch_start);

// Control payload serializers / parsers
size_t write_l2_sync_clock(uint8_t* out, size_t cap, const L2SyncClockPayload& p);
bool parse_l2_sync_clock(const uint8_t* in, size_t len, L2SyncClockPayload& p);

size_t write_l2_discovery(uint8_t* out, size_t cap, const L2DiscoveryPayload& p);
bool parse_l2_discovery(const uint8_t* in, size_t len, L2DiscoveryPayload& p);

size_t write_l2_route_rule(uint8_t* out, size_t cap, const L2RouteRulePayload& p);
bool parse_l2_route_rule(const uint8_t* in, size_t len, L2RouteRulePayload& p);

size_t write_l2_dsp_param(uint8_t* out, size_t cap, const L2DspParamPayload& p);
bool parse_l2_dsp_param(const uint8_t* in, size_t len, L2DspParamPayload& p);

size_t write_l2_heartbeat(uint8_t* out, size_t cap, const L2HeartbeatPayload& p);
bool parse_l2_heartbeat(const uint8_t* in, size_t len, L2HeartbeatPayload& p);

}  // namespace lv::grid
