#pragma once
// Frame Assembly Buffer (4-slot ring) + wrap-aware packet-loss tracker + slice-level XOR FEC recovery
// + slice concealment. Single-owner ingestion (recv thread) with optional async ThreadPool decode.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "codec/slice_codec.hpp"
#include "common/image.hpp"
#include "net/packet.hpp"
#include "parallel/thread_pool.hpp"

namespace lv {

enum class ConcealMode : uint8_t {
    kPrev = 0,  // copy missing/corrupt slice strip from previous output frame (gray if none)
    kGray = 1,  // fill missing/corrupt slice strip with neutral gray (128,128,128)
};

// Sliding-window tracker over 16-bit packet sequence numbers for exact Packet Loss %.
class SeqLossTracker {
public:
    static constexpr int kWindow = 1024;

    void observe(uint16_t seq);

    uint64_t received() const { return received_; }
    uint64_t duplicates() const { return duplicates_; }
    uint64_t reordered() const { return reordered_; }
    uint64_t lost(int reorder_margin = 32) const;
    double loss_percent(int reorder_margin = 32) const;
    void reset();

private:
    bool get_bit(uint16_t seq) const;
    void set_bit(uint16_t seq, bool v);

    bool initialized_ = false;
    uint16_t max_seq_ = 0;
    uint64_t span_ = 0;
    uint64_t received_ = 0;
    uint64_t duplicates_ = 0;
    uint64_t reordered_ = 0;
    uint64_t evicted_lost_ = 0;
    uint64_t bitmap_[kWindow / 64]{};
};

struct AssemblerConfig {
    uint64_t frame_timeout_us = 25000;  // 25 ms default (~1.5x frame interval at 60 fps)
    ConcealMode conceal = ConcealMode::kPrev;
    PixelOrder output_order = PixelOrder::kRGBA;
    bool verify_crc = true;
};

struct CompletedFrame {
    uint32_t frame_id = 0;
    uint32_t sender_timestamp_us = 0;
    uint64_t first_arrival_us = 0;
    uint64_t complete_time_us = 0;
    uint32_t decode_time_us = 0;
    uint16_t total_slices = 0;
    uint16_t slices_ok = 0;
    uint16_t slices_corrupt = 0;
    uint16_t slices_lost = 0;
    uint8_t quality = 0;           // active encoder Q read from slice headers
    const Image* image = nullptr;  // valid for the duration of the callback
};

struct AssemblerStats {
    uint64_t datagrams_total = 0;
    uint64_t bytes_total = 0;
    uint64_t bad_packets = 0;
    uint64_t crc_errors = 0;
    uint64_t late_packets = 0;
    uint64_t fec_parity_received = 0;
    uint64_t fec_recovered_packets = 0;
    uint64_t frames_complete = 0;   // 0 missing/corrupt slices
    uint64_t frames_concealed = 0;  // >= 1 missing/corrupt slice
    uint64_t slices_ok = 0;
    uint64_t slices_lost = 0;
    uint64_t slices_corrupt = 0;
};

class FrameAssembler {
public:
    using FrameCallback = std::function<void(const CompletedFrame&)>;

    explicit FrameAssembler(const AssemblerConfig& cfg, FrameCallback on_frame,
                            ThreadPool* decode_pool = nullptr);
    ~FrameAssembler();

    PacketParseStatus ingest_datagram(const uint8_t* dgram, size_t len, uint64_t now_us);

    void ingest_video_packet(const PacketHeader& h, const uint8_t* payload, size_t dgram_len,
                             uint64_t now_us);

    void record_bad_packet(PacketParseStatus st, size_t dgram_len);

    void poll(uint64_t now_us);
    void flush(uint64_t now_us);

    const AssemblerStats& stats() const { return stats_; }
    const SeqLossTracker& seq_tracker() const { return seq_; }

    void set_conceal_mode(ConcealMode m) { conceal_mode_.store(uint8_t(m), std::memory_order_relaxed); }
    ConcealMode conceal_mode() const { return ConcealMode(conceal_mode_.load(std::memory_order_relaxed)); }

private:
    static constexpr size_t kNumSlots = 4;

    enum class SliceState : uint8_t {
        kEmpty = 0,
        kPartial,
        kComplete,
        kDecoded,
        kCorrupt,
    };

    struct FecParityEntry {
        FecHeader hdr{};
        std::vector<uint8_t> parity;
        bool used = false;
    };

    struct SliceSlot {
        std::atomic<uint8_t> state{0};
        uint16_t frag_count = 0;
        uint16_t frags_received = 0;
        uint16_t chunk_size = 0;
        uint16_t last_frag_size = 0;
        bool has_deferred_last = false;
        size_t total_size = 0;
        std::vector<uint8_t> frag_bitmap;
        std::vector<uint16_t> frag_lens;
        std::vector<uint8_t> buf;
        std::vector<uint8_t> last_frag_tmp;
        std::vector<FecParityEntry> fec_parities;
        SliceHeader hdr{};

        void reset() {
            state.store(uint8_t(SliceState::kEmpty), std::memory_order_relaxed);
            frag_count = 0;
            frags_received = 0;
            chunk_size = 0;
            last_frag_size = 0;
            has_deferred_last = false;
            total_size = 0;
            frag_bitmap.clear();
            frag_lens.clear();
            last_frag_tmp.clear();
            fec_parities.clear();
        }
    };

    struct FrameSlot {
        bool active = false;
        uint32_t frame_id = 0;
        uint32_t sender_ts_us = 0;
        uint64_t first_arrival_us = 0;
        uint16_t total_slices = 0;
        bool has_dims = false;
        int width = 0;
        int height = 0;
        int nominal_slice_h = 16;
        uint8_t quality = 0;
        uint16_t slices_dispatched = 0;
        std::atomic<uint32_t> slices_done{0};
        std::atomic<uint32_t> in_flight{0};
        std::atomic<uint64_t> decode_us_sum{0};
        std::unique_ptr<SliceSlot[]> slices;
        size_t slices_cap = 0;
        Image frame;
    };

    void init_slot(FrameSlot& slot, const PacketHeader& h, uint64_t now_us);
    void ensure_slice_initialized(SliceSlot& ss, uint16_t frag_count);
    void insert_data_fragment(FrameSlot& slot, SliceSlot& ss, uint16_t slice_id, uint16_t fi,
                              const uint8_t* payload, uint16_t payload_size, uint64_t now_us);
    void try_fec_recover(FrameSlot& slot, SliceSlot& ss, uint16_t slice_id, uint64_t now_us);
    void dispatch_slice_decode(FrameSlot& slot, uint16_t slice_id);
    void finalize_slot(FrameSlot& slot, uint64_t now_us);
    void conceal_strip(Image& dst, int y0, int y1) const;
    void try_finalize_ready(uint64_t now_us);
    std::vector<size_t> active_slots_sorted() const;

    AssemblerConfig cfg_;
    std::atomic<uint8_t> conceal_mode_{0};
    FrameCallback on_frame_;
    ThreadPool* pool_ = nullptr;

    struct alignas(64) WorkerScratch {
        SliceScratch scratch;
    };
    std::vector<WorkerScratch> scratches_;

    std::array<FrameSlot, kNumSlots> slots_;
    Image prev_output_;
    int last_known_w_ = 0;
    int last_known_h_ = 0;
    int last_known_slice_h_ = 16;
    uint8_t last_known_quality_ = 0;

    bool has_finalized_ = false;
    uint32_t last_finalized_id_ = 0;

    SeqLossTracker seq_;
    AssemblerStats stats_;
};

}  // namespace lv
