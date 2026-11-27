#include "codec/frame_codec.hpp"

#include <atomic>
#include <cstring>

namespace lv {

void FrameCodec::ensure_workers(uint32_t slots) {
    if (workers_.size() < slots) workers_.resize(slots);
}

const QuantTables& FrameCodec::quant_for(int quality) {
    if (qt_quality_ != quality) {
        build_quant_tables(quality, qt_);
        qt_quality_ = quality;
    }
    return qt_;
}

bool FrameCodec::encode(const Image& src, const CodecParams& params, EncodedFrame& out, ThreadPool* pool) {
    if (src.width != params.width || src.height != params.height) return false;
    Geometry g;
    if (!compute_geometry(params, g)) return false;

    const QuantTables& qt = quant_for(params.quality);
    const size_t cap = max_slice_bytes(g);
    const uint32_t slots = pool ? pool->slot_count() : 1u;
    ensure_workers(slots);

    out.params = params;
    out.geom = g;
    out.slices.resize(size_t(g.total_slices));

    auto work = [&](uint32_t id, uint32_t slot) {
        WorkerBuf& wb = workers_[slot];
        if (wb.out_buf.size() < cap) wb.out_buf.resize(cap);
        const size_t n = encode_slice(src.data.data(), src.stride, params, g, qt, int(id), wb.scratch,
                                      wb.out_buf.data(), cap, src.order);
        auto& dst_vec = out.slices[id];
        dst_vec.resize(n);  // preserves capacity across frames
        std::memcpy(dst_vec.data(), wb.out_buf.data(), n);
    };

    if (pool) {
        pool->parallel_for(uint32_t(g.total_slices), work);
    } else {
        for (uint32_t id = 0; id < uint32_t(g.total_slices); ++id) work(id, 0);
    }
    return true;
}

bool FrameCodec::encode_streaming(const Image& src, const CodecParams& params, const SliceCallback& on_slice,
                                  ThreadPool* pool) {
    if (src.width != params.width || src.height != params.height) return false;
    Geometry g;
    if (!compute_geometry(params, g)) return false;

    const QuantTables& qt = quant_for(params.quality);
    const size_t cap = max_slice_bytes(g);
    const uint32_t slots = pool ? pool->slot_count() : 1u;
    ensure_workers(slots);

    auto work = [&](uint32_t id, uint32_t slot) {
        WorkerBuf& wb = workers_[slot];
        if (wb.out_buf.size() < cap) wb.out_buf.resize(cap);
        const size_t n = encode_slice(src.data.data(), src.stride, params, g, qt, int(id), wb.scratch,
                                      wb.out_buf.data(), cap, src.order);
        on_slice(int(id), g.total_slices, wb.out_buf.data(), n);
    };

    if (pool) {
        pool->parallel_for(uint32_t(g.total_slices), work);
    } else {
        for (uint32_t id = 0; id < uint32_t(g.total_slices); ++id) work(id, 0);
    }
    return true;
}

bool FrameCodec::encode_streaming_yuv420(const Yuv420View& yuv, const CodecParams& params,
                                         const SliceCallback& on_slice, ThreadPool* pool) {
    if (!yuv.y || !yuv.u || !yuv.v || params.width <= 0 || params.height <= 0) return false;
    CodecParams p420 = params;
    p420.chroma = ChromaFormat::k420;
    Geometry g;
    if (!compute_geometry(p420, g)) return false;

    const QuantTables& qt = quant_for(p420.quality);
    const size_t cap = max_slice_bytes(g);
    const uint32_t slots = pool ? pool->slot_count() : 1u;
    ensure_workers(slots);

    auto work = [&](uint32_t id, uint32_t slot) {
        WorkerBuf& wb = workers_[slot];
        if (wb.out_buf.size() < cap) wb.out_buf.resize(cap);
        const size_t n = encode_slice_yuv420(yuv, p420, g, qt, int(id), wb.scratch,
                                             wb.out_buf.data(), cap);
        on_slice(int(id), g.total_slices, wb.out_buf.data(), n);
    };

    if (pool) {
        pool->parallel_for(uint32_t(g.total_slices), work);
    } else {
        for (uint32_t id = 0; id < uint32_t(g.total_slices); ++id) work(id, 0);
    }
    return true;
}

FrameDecodeResult FrameCodec::decode(const EncodedFrame& in, Image& dst, ThreadPool* pool) {
    if (dst.width != in.params.width || dst.height != in.params.height) {
        dst.allocate(in.params.width, in.params.height);
    }

    const uint32_t count = uint32_t(in.slices.size());
    const uint32_t slots = pool ? pool->slot_count() : 1u;
    ensure_workers(slots);

    std::atomic<int> ok{0};
    std::atomic<int> fail{0};

    auto work = [&](uint32_t id, uint32_t slot) {
        const auto& slice = in.slices[id];
        if (slice.empty()) {
            fail.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const DecodeStatus st = decode_slice(slice.data(), slice.size(), workers_[slot].scratch,
                                             dst.data.data(), dst.stride, dst.width, dst.height, nullptr,
                                             dst.order);
        if (st == DecodeStatus::kOk) ok.fetch_add(1, std::memory_order_relaxed);
        else fail.fetch_add(1, std::memory_order_relaxed);
    };

    if (pool) {
        pool->parallel_for(count, work);
    } else {
        for (uint32_t id = 0; id < count; ++id) work(id, 0);
    }
    return {ok.load(), fail.load()};
}

bool encode_frame(const Image& src, const CodecParams& params, EncodedFrame& out, ThreadPool* pool) {
    FrameCodec codec;
    return codec.encode(src, params, out, pool);
}

FrameDecodeResult decode_frame(const EncodedFrame& in, Image& dst, ThreadPool* pool) {
    FrameCodec codec;
    return codec.decode(in, dst, pool);
}

}  // namespace lv
