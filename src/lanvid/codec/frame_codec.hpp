#pragma once
// Whole-frame slice encoder / decoder with reusable per-worker buffers and optional ThreadPool.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "codec/quant.hpp"
#include "codec/slice_codec.hpp"
#include "codec/slice_format.hpp"
#include "common/image.hpp"
#include "parallel/thread_pool.hpp"

namespace lv {

struct EncodedFrame {
    CodecParams params;
    Geometry geom;
    std::vector<std::vector<uint8_t>> slices;

    size_t total_bytes() const {
        size_t n = 0;
        for (const auto& s : slices) n += s.size();
        return n;
    }
};

struct FrameDecodeResult {
    int slices_ok = 0;
    int slices_failed = 0;
};

// Stateful frame codec that reuses per-worker scratch memory across frames (zero hot-path allocs).
class FrameCodec {
public:
    // Encodes all slices of `src` into `out` (in parallel if `pool != nullptr`).
    bool encode(const Image& src, const CodecParams& params, EncodedFrame& out, ThreadPool* pool = nullptr);

    // Encodes all slices and invokes `on_slice(slice_id, total_slices, data, size)` from each worker
    // as soon as a slice finishes (zero extra copy; enables slice-level streaming latency).
    // `on_slice` must be thread-safe when `pool != nullptr`.
    using SliceCallback = std::function<void(int slice_id, int total_slices, const uint8_t* data, size_t size)>;
    bool encode_streaming(const Image& src, const CodecParams& params, const SliceCallback& on_slice,
                          ThreadPool* pool = nullptr);

    // Zero-RGB-conversion streaming encoder for planar/semi-planar YUV_420_888 (e.g. Android Camera2/CameraX).
    bool encode_streaming_yuv420(const Yuv420View& yuv, const CodecParams& params,
                                 const SliceCallback& on_slice, ThreadPool* pool = nullptr);

    // Decodes all slices in `in` into `dst` (in parallel if `pool != nullptr`).
    FrameDecodeResult decode(const EncodedFrame& in, Image& dst, ThreadPool* pool = nullptr);

private:
    struct alignas(64) WorkerBuf {
        SliceScratch scratch;
        std::vector<uint8_t> out_buf;
    };

    void ensure_workers(uint32_t slots);
    const QuantTables& quant_for(int quality);

    std::vector<WorkerBuf> workers_;
    QuantTables qt_;
    int qt_quality_ = -1;
};

// Convenience stateless wrappers (allocate scratch per call; fine for tests/tools).
bool encode_frame(const Image& src, const CodecParams& params, EncodedFrame& out, ThreadPool* pool = nullptr);
FrameDecodeResult decode_frame(const EncodedFrame& in, Image& dst, ThreadPool* pool = nullptr);

}  // namespace lv
