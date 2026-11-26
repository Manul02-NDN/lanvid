#pragma once
// Frame Assembly Buffer (4-slot ring) + wrap-aware packet-loss tracker + slice-level XOR FEC recovery
// + slice concealment. Single-owner ingestion (recv thread) with optional async ThreadPool decode.

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "codec/slice_codec.hpp"
#include "codec/video_decoder.hpp"
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
    // Decode H.264 / H.265 frames on a dedicated worker thread instead of the ingest (network) thread.
    // A hardware decoder session takes ~100-300 ms to open and a software fallback 10-30 ms per frame;
    // on the network thread that stalls AUDIO2 / MIDI / clock packets that share the socket.
    // Frame callbacks then run on the worker thread. flush() drains the worker.
    bool async_inter_decode = false;
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
    uint8_t quality = 0;           // active encoder Q read from slice headers (DCT)
    VideoCodecId codec = VideoCodecId::kLanVidDct;
    bool is_keyframe = false;
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
    uint64_t pli_requests = 0;
    uint64_t frames_dropped_awaiting_idr = 0;
};

class FrameAssembler {
public:
    using FrameCallback = std::function<void(const CompletedFrame&)>;
    using PliCallback = std::function<void(uint32_t failed_frame_id)>;

    explicit FrameAssembler(const AssemblerConfig& cfg, FrameCallback on_frame,
                            ThreadPool* decode_pool = nullptr);
    ~FrameAssembler();

    void set_pli_callback(PliCallback cb) { on_pli_ = std::move(cb); }
    // Complete H.264 / H.265 access units exactly as received (Annex-B), before decoding - for
    // zero-transcode ISO recording. Called on the ingest thread; the data is valid during the call only.
    using BitstreamCallback = std::function<void(VideoCodecId codec, const uint8_t* annexb, size_t size,
                                                 bool is_keyframe, uint32_t frame_id, uint64_t arrival_us)>;
    void set_bitstream_callback(BitstreamCallback cb) { on_bitstream_ = std::move(cb); }

    PacketParseStatus ingest_datagram(const uint8_t* dgram, size_t len, uint64_t now_us);

    void ingest_video_packet(const PacketHeader& h, const uint8_t* payload, size_t dgram_len,
                             uint64_t now_us);

    void record_bad_packet(PacketParseStatus st, size_t dgram_len);

    void poll(uint64_t now_us);
    void flush(uint64_t now_us);

    const AssemblerStats& stats() const { return stats_; }
    const SeqLossTracker& seq_tracker() const { return seq_; }

    VideoCodecId active_codec() const { return active_codec_; }
    std::string decoder_backend_name() const {
        std::lock_guard<std::mutex> lk(backend_mu_);
        return decoder_backend_;
    }
    bool decoder_is_hardware() const { return decoder_is_hw_.load(std::memory_order_relaxed); }
    // Stops the async decode worker (if any). Call before the frame callback's captures go away.
    void stop_decoder_worker();
    bool awaiting_idr() const { return awaiting_idr_; }

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
        VideoCodecId codec = VideoCodecId::kLanVidDct;
        bool is_keyframe = false;
        bool has_codec_config = false;
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
    void finalize_inter_frame_slot(FrameSlot& slot, uint64_t now_us);
    void decode_inter_frame_sync(FrameSlot& slot, uint64_t now_us);
    struct InterJob {
        uint32_t frame_id = 0;
        uint32_t sender_ts_us = 0;
        uint64_t first_arrival_us = 0;
        VideoCodecId codec = VideoCodecId::kH264;
        bool is_keyframe = false;
        std::vector<uint8_t> bits;
    };
    void vdec_loop();
    void vdec_drain();
    void fold_vdec_feedback(uint64_t now_us);
    void set_backend(const std::string& name, bool hw);
    void trigger_pli(uint32_t failed_frame_id, uint64_t now_us, bool immediate);
    void conceal_strip(Image& dst, int y0, int y1) const;
    void try_finalize_ready(uint64_t now_us);
    std::vector<size_t> active_slots_sorted() const;

    AssemblerConfig cfg_;
    std::atomic<uint8_t> conceal_mode_{0};
    FrameCallback on_frame_;
    PliCallback on_pli_;
    BitstreamCallback on_bitstream_;
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

    // Inter-frame (H.264 / H.265) decoder & PLI recovery state
    std::unique_ptr<IVideoDecoder> video_decoder_;  // owned by the worker thread in async mode
    VideoCodecId active_codec_ = VideoCodecId::kLanVidDct;
    mutable std::mutex backend_mu_;
    std::string decoder_backend_ = "LanVid DCT";
    std::atomic<bool> decoder_is_hw_{false};

    // Async inter-frame decode worker (AssemblerConfig::async_inter_decode).
    static constexpr size_t kMaxInterQueue = 16;  // ~270 ms @ 60 fps: covers a decoder session start-up
    std::thread vdec_thread_;
    std::mutex vdec_mu_;
    std::condition_variable vdec_cv_;
    std::condition_variable vdec_idle_cv_;
    std::deque<InterJob> vdec_q_;
    bool vdec_busy_ = false;
    bool vdec_stop_ = false;
    bool vdec_need_key_ = true;                // worker: fresh / failed decoder waits for an IDR
    Image vdec_frame_;                         // worker-owned output image
    std::atomic<bool> vdec_error_{false};      // worker -> ingest thread: decode failed, request IDR
    std::atomic<uint32_t> vdec_error_fid_{0};
    std::atomic<uint64_t> vdec_ok_{0}, vdec_fail_{0}, vdec_skipped_{0};
    std::atomic<int> vdec_w_{0}, vdec_h_{0};
    uint64_t vdec_ok_seen_ = 0, vdec_fail_seen_ = 0, vdec_skipped_seen_ = 0;
    bool has_submitted_inter_ = false;         // ingest thread: frame-gap tracking in async mode
    uint32_t last_submitted_fid_ = 0;
    bool awaiting_idr_ = false;
    bool has_decoded_inter_frame_ = false;
    uint32_t last_decoded_inter_fid_ = 0;
    uint64_t last_pli_us_ = 0;

    SeqLossTracker seq_;
    AssemblerStats stats_;
};

}  // namespace lv
