#include "video/iso_recorder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>

namespace lv {

const char* iso_container_extension(IsoContainer c) {
    switch (c) {
        case IsoContainer::kMkv: return "mkv";
        case IsoContainer::kMov: return "mov";
        default: return "mp4";
    }
}

// ---------------------------------------------------------------------------------------------------
// Bitstream helpers
// ---------------------------------------------------------------------------------------------------
std::vector<std::pair<const uint8_t*, size_t>> annexb_split(const uint8_t* d, size_t n) {
    std::vector<std::pair<const uint8_t*, size_t>> out;
    size_t i = 0, start = SIZE_MAX;
    while (i + 2 < n) {
        if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (i + 3 < n && d[i + 2] == 0 && d[i + 3] == 1))) {
            const size_t sc = d[i + 2] == 1 ? 3 : 4;
            if (start != SIZE_MAX) {
                size_t end = i;
                while (end > start && d[end - 1] == 0) --end;  // trailing_zero_8bits
                if (end > start) out.emplace_back(d + start, end - start);
            }
            i += sc;
            start = i;
        } else {
            ++i;
        }
    }
    if (start != SIZE_MAX && start < n) out.emplace_back(d + start, n - start);
    if (start == SIZE_MAX && n) out.emplace_back(d, n);  // no start code: one raw NAL
    return out;
}

namespace {
std::vector<uint8_t> unescape_rbsp(const uint8_t* d, size_t n) {
    std::vector<uint8_t> o;
    o.reserve(n);
    int zeros = 0;
    for (size_t i = 0; i < n; ++i) {
        if (zeros >= 2 && d[i] == 3) {
            zeros = 0;
            continue;
        }
        o.push_back(d[i]);
        zeros = d[i] == 0 ? zeros + 1 : 0;
    }
    return o;
}

struct BitReader {
    const std::vector<uint8_t>& b;
    size_t pos = 0;
    explicit BitReader(const std::vector<uint8_t>& v) : b(v) {}
    bool eof() const { return pos >= b.size() * 8; }
    uint32_t bit() {
        if (eof()) return 0;
        const uint32_t v = (b[pos >> 3] >> (7 - (pos & 7))) & 1u;
        ++pos;
        return v;
    }
    uint32_t bits(int n) {
        uint32_t v = 0;
        while (n--) v = (v << 1) | bit();
        return v;
    }
    uint32_t ue() {
        int lz = 0;
        while (!eof() && bit() == 0 && lz < 32) ++lz;
        return lz ? ((1u << lz) - 1u + bits(lz)) : 0u;
    }
    int32_t se() {
        const uint32_t k = ue();
        return (k & 1) ? int32_t((k + 1) / 2) : -int32_t(k / 2);
    }
};

void skip_scaling_list(BitReader& br, int size) {
    int last = 8, next = 8;
    for (int j = 0; j < size; ++j) {
        if (next != 0) next = (last + br.se() + 256) % 256;
        last = next == 0 ? last : next;
    }
}
}  // namespace

bool h264_sps_dimensions(const uint8_t* sps, size_t size, int& width, int& height) {
    if (!sps || size < 4) return false;
    const auto rb = unescape_rbsp(sps + 1, size - 1);  // skip NAL header
    BitReader br(rb);
    const uint32_t profile = br.bits(8);
    br.bits(16);  // constraint flags + level
    br.ue();      // sps id
    uint32_t chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134) {
        chroma = br.ue();
        if (chroma == 3) br.bit();
        br.ue();
        br.ue();
        br.bit();
        if (br.bit()) {
            for (int i = 0; i < (chroma != 3 ? 8 : 12); ++i)
                if (br.bit()) skip_scaling_list(br, i < 6 ? 16 : 64);
        }
    }
    br.ue();  // log2_max_frame_num
    const uint32_t poc_type = br.ue();
    if (poc_type == 0) {
        br.ue();
    } else if (poc_type == 1) {
        br.bit();
        br.se();
        br.se();
        const uint32_t n = br.ue();
        for (uint32_t i = 0; i < n && i < 256; ++i) br.se();
    }
    br.ue();  // max_num_ref_frames
    br.bit();
    const uint32_t wmb = br.ue() + 1, hmu = br.ue() + 1;
    const uint32_t frame_mbs_only = br.bit();
    if (!frame_mbs_only) br.bit();
    br.bit();
    uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
    if (br.bit()) {
        cl = br.ue();
        cr = br.ue();
        ct = br.ue();
        cb = br.ue();
    }
    const uint32_t sub_w = (chroma == 1 || chroma == 2) ? 2 : 1;
    const uint32_t sub_h = (chroma == 1) ? 2 : 1;
    const uint32_t crop_x = chroma ? sub_w : 1, crop_y = (chroma ? sub_h : 1) * (2 - frame_mbs_only);
    const int64_t w = int64_t(wmb) * 16 - int64_t(crop_x) * (cl + cr);
    const int64_t h = int64_t(hmu) * 16 * (2 - frame_mbs_only) - int64_t(crop_y) * (ct + cb);
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
    width = int(w);
    height = int(h);
    return true;
}

std::string iso_timecode_string(uint64_t frames, int fps) {
    fps = std::max(1, fps);
    const uint64_t f = frames % uint64_t(fps), s = frames / uint64_t(fps);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u:%02u", unsigned((s / 3600) % 24), unsigned((s / 60) % 60),
                  unsigned(s % 60), unsigned(f));
    return buf;
}

uint64_t iso_timecode_now(int fps) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    std::tm lt{};
#if defined(_WIN32)
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    const uint64_t secs = uint64_t(lt.tm_hour) * 3600 + uint64_t(lt.tm_min) * 60 + uint64_t(lt.tm_sec);
    const uint64_t ms = uint64_t(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    return secs * uint64_t(fps) + ms * uint64_t(fps) / 1000;
}

// ---------------------------------------------------------------------------------------------------
// Byte writers
// ---------------------------------------------------------------------------------------------------
namespace {
struct Buf {
    std::vector<uint8_t> v;
    void u8(uint32_t x) { v.push_back(uint8_t(x)); }
    void u16(uint32_t x) { u8(x >> 8); u8(x); }
    void u24(uint32_t x) { u8(x >> 16); u16(x); }
    void u32(uint32_t x) { u16(x >> 16); u16(x); }
    void u64(uint64_t x) { u32(uint32_t(x >> 32)); u32(uint32_t(x)); }
    void tag(const char* t) { v.insert(v.end(), t, t + 4); }
    void bytes(const void* p, size_t n) { v.insert(v.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    void zeros(size_t n) { v.insert(v.end(), n, 0); }
    size_t box(const char* t) { const size_t at = v.size(); u32(0); tag(t); return at; }
    size_t fullbox(const char* t, uint8_t ver, uint32_t flags) { const size_t at = box(t); u8(ver); u24(flags); return at; }
    void end(size_t at) {
        const uint32_t sz = uint32_t(v.size() - at);
        v[at] = uint8_t(sz >> 24); v[at + 1] = uint8_t(sz >> 16); v[at + 2] = uint8_t(sz >> 8); v[at + 3] = uint8_t(sz);
    }
    void append(const Buf& o) { v.insert(v.end(), o.v.begin(), o.v.end()); }
};

uint32_t h264_type(const uint8_t* n) { return n[0] & 0x1Fu; }
uint32_t hevc_type(const uint8_t* n) { return (n[0] >> 1) & 0x3Fu; }

std::vector<uint8_t> build_avcc(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps) {
    Buf b;
    b.u8(1);
    b.u8(sps.size() > 1 ? sps[1] : 0x64);
    b.u8(sps.size() > 2 ? sps[2] : 0);
    b.u8(sps.size() > 3 ? sps[3] : 0x28);
    b.u8(0xFF);  // 4-byte NAL lengths
    b.u8(0xE1);
    b.u16(uint32_t(sps.size()));
    b.bytes(sps.data(), sps.size());
    b.u8(1);
    b.u16(uint32_t(pps.size()));
    b.bytes(pps.data(), pps.size());
    return b.v;
}

std::vector<uint8_t> build_hvcc(const std::vector<uint8_t>& vps, const std::vector<uint8_t>& sps,
                                const std::vector<uint8_t>& pps) {
    // general_profile_tier_level (12 bytes) sits right after the 2-byte NAL header + 1 byte in the SPS.
    std::vector<uint8_t> ptl(12, 0);
    const auto rb = unescape_rbsp(sps.data(), sps.size());
    if (rb.size() >= 15) std::copy(rb.begin() + 3, rb.begin() + 15, ptl.begin());
    Buf b;
    b.u8(1);
    b.bytes(ptl.data(), 1 + 4 + 6);  // profile_space/tier/idc, compat flags, constraint flags
    b.u8(ptl[11]);                   // level
    b.u16(0xF000);                   // min_spatial_segmentation_idc
    b.u8(0xFC);                      // parallelismType
    b.u8(0xFC | 1);                  // chroma 4:2:0
    b.u8(0xF8);                      // luma 8 bit
    b.u8(0xF8);                      // chroma 8 bit
    b.u16(0);                        // avgFrameRate
    b.u8((1 << 3) | (1 << 2) | 3);   // numTemporalLayers=1, temporalIdNested, lengthSizeMinusOne=3
    b.u8(3);
    const std::vector<uint8_t>* arr[3] = {&vps, &sps, &pps};
    const uint8_t types[3] = {32, 33, 34};
    for (int i = 0; i < 3; ++i) {
        b.u8(0x80 | types[i]);
        b.u16(1);
        b.u16(uint32_t(arr[i]->size()));
        b.bytes(arr[i]->data(), arr[i]->size());
    }
    return b.v;
}

uint64_t mp4_time_now() {  // seconds since 1904-01-01
    return uint64_t(std::time(nullptr)) + 2082844800ull;
}
}  // namespace

// ---------------------------------------------------------------------------------------------------
// Container implementation
// ---------------------------------------------------------------------------------------------------
struct IsoRecorder::Impl {
    IsoRecorderConfig cfg;
    FILE* f = nullptr;
    bool io_error = false;
    uint64_t file_pos = 0;

    // parameter sets
    std::vector<uint8_t> vps, sps, pps;
    bool started = false;  // first keyframe written
    uint64_t first_pts_us = 0;
    uint64_t last_video_pts_us = 0;

    // MP4 / MOV sample tables
    uint64_t mdat_header_pos = 0;
    uint64_t tmcd_sample_pos = 0;
    std::vector<uint32_t> v_sizes;
    std::vector<uint64_t> v_offsets;
    std::vector<uint64_t> v_pts;  // us, relative to first_pts
    std::vector<uint32_t> v_sync;  // 1-based sample numbers
    std::vector<uint64_t> a_chunk_offsets;
    std::vector<uint32_t> a_chunk_frames;
    uint64_t a_total_frames = 0;

    // MKV
    uint64_t seg_data_pos = 0;     // file offset of the first byte inside Segment
    uint64_t seg_size_pos = 0;
    uint64_t duration_pos = 0;
    bool tracks_written = false;
    Buf cluster;
    uint64_t cluster_ts_ms = 0;
    bool cluster_open = false;
    struct Cue { uint64_t ts_ms; uint64_t pos; };
    std::vector<Cue> cues;

    IsoRecorderStats st;

    bool put(const void* p, size_t n) {
        if (!f || io_error) return false;
        if (n && std::fwrite(p, 1, n, f) != n) {
            io_error = true;
            return false;
        }
        file_pos += n;
        st.bytes_written = file_pos;
        return true;
    }
    bool put(const Buf& b) { return put(b.v.data(), b.v.size()); }
    bool patch(uint64_t pos, const void* p, size_t n) {
        if (!f) return false;
        std::fflush(f);
#if defined(_WIN32)
        if (_fseeki64(f, int64_t(pos), SEEK_SET) != 0) return false;
#else
        if (fseeko(f, off_t(pos), SEEK_SET) != 0) return false;
#endif
        const bool ok = std::fwrite(p, 1, n, f) == n;
#if defined(_WIN32)
        _fseeki64(f, 0, SEEK_END);
#else
        fseeko(f, 0, SEEK_END);
#endif
        return ok;
    }
    bool mp4() const { return cfg.container != IsoContainer::kMkv; }
    bool mov() const { return cfg.container == IsoContainer::kMov; }
    bool hevc() const { return cfg.codec == IsoVideoCodec::kH265; }

    // ---------------- open ----------------
    bool open() {
#if defined(_WIN32)
        f = std::fopen(cfg.path.c_str(), "wb");  // UTF-8 paths: caller passes ASCII-safe names (sanitized label)
#else
        f = std::fopen(cfg.path.c_str(), "wb");
#endif
        if (!f) return false;
        if (mp4()) return open_mp4();
        return open_mkv();
    }

    bool open_mp4() {
        Buf b;
        const size_t ftyp = b.box("ftyp");
        if (mov()) {
            b.tag("qt  ");
            b.u32(0x20050300);
            b.tag("qt  ");
        } else {
            b.tag("isom");
            b.u32(0x200);
            b.tag("isom");
            b.tag("iso2");
            b.tag(hevc() ? "hvc1" : "avc1");
            b.tag("mp41");
        }
        b.end(ftyp);
        // mdat with 64-bit size (patched on close)
        mdat_header_pos = b.v.size();
        b.u32(1);
        b.tag("mdat");
        b.u64(0);
        if (!put(b)) return false;
        // timecode sample (4-byte big-endian frame number) lives at the start of mdat
        tmcd_sample_pos = file_pos;
        Buf t;
        t.u32(uint32_t(cfg.timecode_start_frame));
        return put(t);
    }

    // ---------------- video ----------------
    bool write_video(const uint8_t* data, size_t size, bool key, uint64_t pts_us) {
        auto nals = annexb_split(data, size);
        // pick up parameter sets (in-band, every IDR from LanVid encoders)
        for (auto& [p, n] : nals) {
            if (!n) continue;
            if (hevc()) {
                const uint32_t t = hevc_type(p);
                if (t == 32) vps.assign(p, p + n);
                else if (t == 33) sps.assign(p, p + n);
                else if (t == 34) pps.assign(p, p + n);
                if (t >= 16 && t <= 21) key = true;
            } else {
                const uint32_t t = h264_type(p);
                if (t == 7) sps.assign(p, p + n);
                else if (t == 8) pps.assign(p, p + n);
                if (t == 5) key = true;
            }
        }
        if (!started) {
            const bool have_ps = !sps.empty() && !pps.empty() && (!hevc() || !vps.empty());
            if (!key || !have_ps) {
                st.waiting_for_keyframe = true;
                return true;  // skip until a decodable start
            }
            if (!hevc()) {
                int w = 0, h = 0;
                if (h264_sps_dimensions(sps.data(), sps.size(), w, h)) {
                    cfg.width = w;
                    cfg.height = h;
                }
            }
            started = true;
            st.waiting_for_keyframe = false;
            first_pts_us = pts_us;
            if (!mp4() && !write_mkv_headers()) return false;
        }
        const uint64_t rel = pts_us >= first_pts_us ? pts_us - first_pts_us : 0;
        // length-prefixed sample without parameter sets / AUD
        Buf s;
        for (auto& [p, n] : nals) {
            if (!n) continue;
            const uint32_t t = hevc() ? hevc_type(p) : h264_type(p);
            const bool drop = hevc() ? (t == 32 || t == 33 || t == 34 || t == 35) : (t == 7 || t == 8 || t == 9);
            if (drop) continue;
            s.u32(uint32_t(n));
            s.bytes(p, n);
        }
        if (s.v.empty()) return true;
        last_video_pts_us = std::max(last_video_pts_us, rel);
        ++st.video_frames;
        st.duration_us = last_video_pts_us;
        if (mp4()) {
            v_offsets.push_back(file_pos);
            v_sizes.push_back(uint32_t(s.v.size()));
            v_pts.push_back(rel);
            if (key) v_sync.push_back(uint32_t(v_sizes.size()));
            return put(s);
        }
        return mkv_block(1, rel / 1000, key, s.v.data(), s.v.size());
    }

    // ---------------- audio ----------------
    bool write_audio(const int16_t* pcm, uint32_t frames, uint64_t pts_us) {
        if (!cfg.audio_channels || !frames || !started) return true;  // audio starts with the first video frame
        const size_t bytes = size_t(frames) * cfg.audio_channels * 2;
        st.audio_frames += frames;
        // samples are little-endian on all supported hosts
        if (mp4()) {
            a_chunk_offsets.push_back(file_pos);
            a_chunk_frames.push_back(frames);
            a_total_frames += frames;
            return put(pcm, bytes);
        }
        const uint64_t rel = pts_us >= first_pts_us ? pts_us - first_pts_us : 0;
        return mkv_block(2, rel / 1000, true, reinterpret_cast<const uint8_t*>(pcm), bytes);
    }

    // ---------------- close ----------------
    bool close() {
        if (!f) return !io_error;
        bool ok = true;
        if (mp4()) ok = close_mp4();
        else ok = close_mkv();
        std::fflush(f);
        std::fclose(f);
        f = nullptr;
        return ok && !io_error;
    }

    uint32_t video_timescale() const { return 90000; }

    bool close_mp4() {
        // patch mdat size
        const uint64_t mdat_size = file_pos - mdat_header_pos;
        uint8_t be[8];
        for (int i = 0; i < 8; ++i) be[i] = uint8_t(mdat_size >> (56 - 8 * i));
        patch(mdat_header_pos + 8, be, 8);

        const uint32_t vts = video_timescale();
        const size_t nv = v_sizes.size();
        // per-sample durations from timestamps (last one = previous delta / 1/30 s)
        std::vector<uint32_t> durs(nv, vts / 30);
        uint64_t v_dur_ts = 0;
        for (size_t i = 0; i < nv; ++i) {
            if (i + 1 < nv) {
                const uint64_t d = (v_pts[i + 1] - v_pts[i]) * vts / 1000000ull;
                durs[i] = uint32_t(std::clamp<uint64_t>(d, 1, vts));
            } else if (i > 0) {
                durs[i] = durs[i - 1];
            }
            v_dur_ts += durs[i];
        }
        const uint64_t movie_dur_ms = v_dur_ts * 1000 / vts;
        const bool has_audio = cfg.audio_channels > 0 && a_total_frames > 0;
        const bool has_tmcd = nv > 0;
        const uint64_t ctime = cfg.creation_unix_s ? cfg.creation_unix_s + 2082844800ull : mp4_time_now();
        const uint32_t next_track = 4;

        Buf m;
        const size_t moov = m.box("moov");
        {
            const size_t mvhd = m.fullbox("mvhd", 1, 0);
            m.u64(ctime);
            m.u64(ctime);
            m.u32(1000);
            m.u64(movie_dur_ms);
            m.u32(0x00010000);
            m.u16(0x0100);
            m.zeros(10);
            const uint32_t matrix[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
            for (uint32_t x : matrix) m.u32(x);
            m.zeros(24);
            m.u32(next_track);
            m.end(mvhd);
        }
        auto tkhd = [&](uint32_t id, uint64_t dur_ms, bool audio, uint32_t w, uint32_t h, uint32_t flags) {
            const size_t t = m.fullbox("tkhd", 1, flags);
            m.u64(ctime);
            m.u64(ctime);
            m.u32(id);
            m.u32(0);
            m.u64(dur_ms);
            m.zeros(8);
            m.u16(0);
            m.u16(audio ? 1 : 0);  // alternate group
            m.u16(audio ? 0x0100 : 0);
            m.u16(0);
            const uint32_t matrix[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
            for (uint32_t x : matrix) m.u32(x);
            m.u32(w << 16);
            m.u32(h << 16);
            m.end(t);
        };
        auto mdhd = [&](uint32_t ts, uint64_t dur) {
            const size_t t = m.fullbox("mdhd", 1, 0);
            m.u64(ctime);
            m.u64(ctime);
            m.u32(ts);
            m.u64(dur);
            m.u16(0x55C4);  // 'und'
            m.u16(0);
            m.end(t);
        };
        auto hdlr = [&](const char* type, const char* name, bool mhlr) {
            const size_t t = m.fullbox("hdlr", 0, 0);
            if (mhlr) m.tag("mhlr");
            else m.u32(0);
            m.tag(type);
            m.zeros(12);
            if (mov()) m.u8(uint32_t(std::strlen(name)));  // QuickTime: Pascal string
            m.bytes(name, std::strlen(name));
            if (!mov()) m.u8(0);
            m.end(t);
        };
        auto dinf = [&]() {
            const size_t d = m.box("dinf");
            const size_t dr = m.fullbox("dref", 0, 0);
            m.u32(1);
            const size_t u = m.fullbox("url ", 0, 1);
            m.end(u);
            m.end(dr);
            m.end(d);
        };
        auto co64 = [&](const std::vector<uint64_t>& offs) {
            const size_t c = m.fullbox("co64", 0, 0);
            m.u32(uint32_t(offs.size()));
            for (uint64_t o : offs) m.u64(o);
            m.end(c);
        };

        // ---- video track ----
        if (nv) {
            const size_t trak = m.box("trak");
            tkhd(1, movie_dur_ms, false, uint32_t(cfg.width), uint32_t(cfg.height), 3);
            if (has_tmcd) {
                const size_t tref = m.box("tref");
                const size_t tm = m.box("tmcd");
                m.u32(3);
                m.end(tm);
                m.end(tref);
            }
            const size_t mdia = m.box("mdia");
            mdhd(vts, v_dur_ts);
            hdlr("vide", "LanVid Video", mov());
            const size_t minf = m.box("minf");
            const size_t vmhd = m.fullbox("vmhd", 0, 1);
            m.zeros(8);
            m.end(vmhd);
            dinf();
            const size_t stbl = m.box("stbl");
            {
                const size_t stsd = m.fullbox("stsd", 0, 0);
                m.u32(1);
                const size_t se = m.box(hevc() ? "hvc1" : "avc1");
                m.zeros(6);
                m.u16(1);
                m.zeros(16);
                m.u16(uint32_t(cfg.width));
                m.u16(uint32_t(cfg.height));
                m.u32(0x00480000);
                m.u32(0x00480000);
                m.u32(0);
                m.u16(1);
                char comp[32] = {};
                const char* cname = hevc() ? "LanVid HEVC" : "LanVid H.264";
                comp[0] = char(std::strlen(cname));
                std::memcpy(comp + 1, cname, std::strlen(cname));
                m.bytes(comp, 32);
                m.u16(0x0018);
                m.u16(0xFFFF);
                const auto cfgrec = hevc() ? build_hvcc(vps, sps, pps) : build_avcc(sps, pps);
                const size_t cb = m.box(hevc() ? "hvcC" : "avcC");
                m.bytes(cfgrec.data(), cfgrec.size());
                m.end(cb);
                m.end(se);
                m.end(stsd);
            }
            {
                // stts (run-length of equal durations)
                std::vector<std::pair<uint32_t, uint32_t>> runs;
                for (uint32_t d : durs) {
                    if (!runs.empty() && runs.back().second == d) ++runs.back().first;
                    else runs.emplace_back(1, d);
                }
                const size_t stts = m.fullbox("stts", 0, 0);
                m.u32(uint32_t(runs.size()));
                for (auto& r : runs) {
                    m.u32(r.first);
                    m.u32(r.second);
                }
                m.end(stts);
            }
            if (v_sync.size() != nv) {
                const size_t stss = m.fullbox("stss", 0, 0);
                m.u32(uint32_t(v_sync.size()));
                for (uint32_t s : v_sync) m.u32(s);
                m.end(stss);
            }
            {
                const size_t stsc = m.fullbox("stsc", 0, 0);
                m.u32(1);
                m.u32(1);
                m.u32(1);
                m.u32(1);
                m.end(stsc);
                const size_t stsz = m.fullbox("stsz", 0, 0);
                m.u32(0);
                m.u32(uint32_t(nv));
                for (uint32_t s : v_sizes) m.u32(s);
                m.end(stsz);
                co64(v_offsets);
            }
            m.end(stbl);
            m.end(minf);
            m.end(mdia);
            m.end(trak);
        }

        // ---- audio track (PCM16) ----
        if (has_audio) {
            const uint32_t sr = cfg.audio_sample_rate;
            const size_t trak = m.box("trak");
            tkhd(2, a_total_frames * 1000 / sr, true, 0, 0, 3);
            const size_t mdia = m.box("mdia");
            mdhd(sr, a_total_frames);
            hdlr("soun", "LanVid Audio", mov());
            const size_t minf = m.box("minf");
            const size_t smhd = m.fullbox("smhd", 0, 0);
            m.u32(0);
            m.end(smhd);
            dinf();
            const size_t stbl = m.box("stbl");
            const size_t stsd = m.fullbox("stsd", 0, 0);
            m.u32(1);
            const size_t se = m.box(mov() ? "sowt" : "ipcm");
            m.zeros(6);
            m.u16(1);
            m.zeros(8);
            m.u16(cfg.audio_channels);
            m.u16(16);
            m.u16(0);
            m.u16(0);
            m.u32(sr << 16);
            if (!mov()) {
                const size_t pc = m.fullbox("pcmC", 0, 0);
                m.u8(1);   // little endian
                m.u8(16);
                m.end(pc);
            }
            m.end(se);
            m.end(stsd);
            const size_t stts = m.fullbox("stts", 0, 0);
            m.u32(1);
            m.u32(uint32_t(a_total_frames));
            m.u32(1);
            m.end(stts);
            // stsc: runs of equal chunk sizes
            std::vector<std::pair<uint32_t, uint32_t>> runs;  // first_chunk, frames
            for (size_t i = 0; i < a_chunk_frames.size(); ++i)
                if (runs.empty() || runs.back().second != a_chunk_frames[i]) runs.emplace_back(uint32_t(i + 1), a_chunk_frames[i]);
            const size_t stsc = m.fullbox("stsc", 0, 0);
            m.u32(uint32_t(runs.size()));
            for (auto& r : runs) {
                m.u32(r.first);
                m.u32(r.second);
                m.u32(1);
            }
            m.end(stsc);
            const size_t stsz = m.fullbox("stsz", 0, 0);
            m.u32(uint32_t(cfg.audio_channels) * 2);
            m.u32(uint32_t(a_total_frames));
            m.end(stsz);
            co64(a_chunk_offsets);
            m.end(stbl);
            m.end(minf);
            m.end(mdia);
            m.end(trak);
        }

        // ---- timecode track ----
        if (has_tmcd) {
            const uint32_t fps = uint32_t(std::max(1, cfg.timecode_fps));
            const uint64_t tc_dur = std::max<uint64_t>(1, v_dur_ts * fps / vts);
            const size_t trak = m.box("trak");
            tkhd(3, movie_dur_ms, false, 0, 0, 3);
            const size_t mdia = m.box("mdia");
            mdhd(fps, tc_dur);
            hdlr("tmcd", "LanVid Timecode", mov());
            const size_t minf = m.box("minf");
            {
                const size_t gmhd = m.box("gmhd");
                const size_t gmin = m.fullbox("gmin", 0, 0);
                m.u16(0x40);
                m.u16(0x8000);
                m.u16(0x8000);
                m.u16(0x8000);
                m.u16(0);
                m.u16(0);
                m.end(gmin);
                const size_t tm = m.box("tmcd");
                const size_t tcmi = m.fullbox("tcmi", 0, 0);
                m.u16(0);
                m.u16(0);
                m.u16(12);
                m.u16(0);
                m.u16(0xFFFF); m.u16(0xFFFF); m.u16(0xFFFF);
                m.u16(0); m.u16(0); m.u16(0);
                const char* font = "System";
                m.u8(uint32_t(std::strlen(font)));
                m.bytes(font, std::strlen(font));
                m.end(tcmi);
                m.end(tm);
                m.end(gmhd);
            }
            dinf();
            const size_t stbl = m.box("stbl");
            const size_t stsd = m.fullbox("stsd", 0, 0);
            m.u32(1);
            const size_t se = m.box("tmcd");
            m.zeros(6);
            m.u16(1);
            m.u32(0);        // reserved
            m.u32(0x02);     // flags: 24-hour max (no drop frame)
            m.u32(fps);      // timescale
            m.u32(1);        // frame duration
            m.u8(fps);       // number of frames
            m.u8(0);
            m.end(se);
            m.end(stsd);
            const size_t stts = m.fullbox("stts", 0, 0);
            m.u32(1);
            m.u32(1);
            m.u32(uint32_t(tc_dur));
            m.end(stts);
            const size_t stsc = m.fullbox("stsc", 0, 0);
            m.u32(1);
            m.u32(1);
            m.u32(1);
            m.u32(1);
            m.end(stsc);
            const size_t stsz = m.fullbox("stsz", 0, 0);
            m.u32(4);
            m.u32(1);
            m.end(stsz);
            co64({tmcd_sample_pos});
            m.end(stbl);
            m.end(minf);
            m.end(mdia);
            m.end(trak);
        }
        m.end(moov);
        return put(m);
    }

    // ---------------- Matroska ----------------
    static void ebml_id(Buf& b, uint32_t id) {
        if (id >= 0x1000000) b.u32(id);
        else if (id >= 0x10000) b.u24(id);
        else if (id >= 0x100) b.u16(id);
        else b.u8(id);
    }
    static void ebml_size(Buf& b, uint64_t n) {  // always 8-byte sizes: simple and patchable
        b.u8(0x01);
        for (int i = 6; i >= 0; --i) b.u8(uint32_t(n >> (8 * i)));
    }
    static void ebml_uint(Buf& b, uint32_t id, uint64_t v) {
        ebml_id(b, id);
        int n = 1;
        while (n < 8 && (v >> (8 * n))) ++n;
        b.u8(0x80 | n);
        for (int i = n - 1; i >= 0; --i) b.u8(uint32_t(v >> (8 * i)));
    }
    static void ebml_str(Buf& b, uint32_t id, const std::string& s) {
        ebml_id(b, id);
        ebml_size(b, s.size());
        b.bytes(s.data(), s.size());
    }
    static void ebml_bin(Buf& b, uint32_t id, const std::vector<uint8_t>& d) {
        ebml_id(b, id);
        ebml_size(b, d.size());
        b.bytes(d.data(), d.size());
    }
    static void ebml_float(Buf& b, uint32_t id, double v) {
        ebml_id(b, id);
        b.u8(0x88);
        uint64_t u;
        std::memcpy(&u, &v, 8);
        b.u64(u);
    }
    static size_t ebml_master(Buf& b, uint32_t id) {
        ebml_id(b, id);
        const size_t at = b.v.size();
        ebml_size(b, 0);
        return at;
    }
    static void ebml_end(Buf& b, size_t at) {
        const uint64_t n = b.v.size() - at - 8;
        b.v[at] = 0x01;
        for (int i = 0; i < 7; ++i) b.v[at + 1 + i] = uint8_t(n >> (8 * (6 - i)));
    }

    bool open_mkv() {
        Buf b;
        const size_t ebml = ebml_master(b, 0x1A45DFA3);
        ebml_uint(b, 0x4286, 1);
        ebml_uint(b, 0x42F7, 1);
        ebml_uint(b, 0x42F2, 4);
        ebml_uint(b, 0x42F3, 8);
        ebml_str(b, 0x4282, "matroska");
        ebml_uint(b, 0x4287, 4);
        ebml_uint(b, 0x4285, 2);
        ebml_end(b, ebml);
        ebml_id(b, 0x18538067);  // Segment, size patched on close
        seg_size_pos = b.v.size();
        ebml_size(b, 0x00FFFFFFFFFFFFFFull);
        if (!put(b)) return false;
        seg_data_pos = file_pos;
        Buf info;
        const size_t in = ebml_master(info, 0x1549A966);
        ebml_uint(info, 0x2AD7B1, 1000000);  // 1 ms timestamps
        ebml_str(info, 0x4D80, "LanVid ISO Recorder");
        ebml_str(info, 0x5741, "LanVid Video");
        if (!cfg.camera_label.empty()) ebml_str(info, 0x7BA9, cfg.camera_label);
        {
            const int64_t unix_s = int64_t(cfg.creation_unix_s ? cfg.creation_unix_s : uint64_t(std::time(nullptr)));
            const int64_t ns = (unix_s - 978307200LL) * 1000000000LL;  // since 2001-01-01
            ebml_id(info, 0x4461);
            info.u8(0x88);
            info.u64(uint64_t(ns));
        }
        duration_pos = file_pos + info.v.size() + 3;  // 0x4489 (2 bytes) + 0x88 (1 byte)
        ebml_float(info, 0x4489, 0.0);
        ebml_end(info, in);
        return put(info);
    }

    bool write_mkv_headers() {
        Buf b;
        const size_t tracks = ebml_master(b, 0x1654AE6B);
        {
            const size_t te = ebml_master(b, 0xAE);
            ebml_uint(b, 0xD7, 1);
            ebml_uint(b, 0x73C5, 0x1A2B3C01);
            ebml_uint(b, 0x83, 1);
            ebml_uint(b, 0x9C, 0);
            ebml_str(b, 0x86, hevc() ? "V_MPEGH/ISO/HEVC" : "V_MPEG4/ISO/AVC");
            ebml_bin(b, 0x63A2, hevc() ? build_hvcc(vps, sps, pps) : build_avcc(sps, pps));
            const size_t vid = ebml_master(b, 0xE0);
            ebml_uint(b, 0xB0, uint64_t(cfg.width));
            ebml_uint(b, 0xBA, uint64_t(cfg.height));
            ebml_end(b, vid);
            ebml_end(b, te);
        }
        if (cfg.audio_channels) {
            const size_t te = ebml_master(b, 0xAE);
            ebml_uint(b, 0xD7, 2);
            ebml_uint(b, 0x73C5, 0x1A2B3C02);
            ebml_uint(b, 0x83, 2);
            ebml_uint(b, 0x9C, 0);
            ebml_str(b, 0x86, "A_PCM/INT/LIT");
            const size_t au = ebml_master(b, 0xE1);
            ebml_float(b, 0xB5, double(cfg.audio_sample_rate));
            ebml_uint(b, 0x9F, cfg.audio_channels);
            ebml_uint(b, 0x6264, 16);
            ebml_end(b, au);
            ebml_end(b, te);
        }
        ebml_end(b, tracks);
        // Tags: session start timecode (same for every camera of the session)
        const size_t tags = ebml_master(b, 0x1254C367);
        const size_t tag = ebml_master(b, 0x7373);
        const size_t tg = ebml_master(b, 0x63C0);
        ebml_end(b, tg);
        const size_t st1 = ebml_master(b, 0x67C8);
        ebml_str(b, 0x45A3, "TIMECODE");
        ebml_str(b, 0x4487, iso_timecode_string(cfg.timecode_start_frame, cfg.timecode_fps));
        ebml_end(b, st1);
        ebml_end(b, tag);
        ebml_end(b, tags);
        tracks_written = true;
        return put(b);
    }

    bool flush_cluster() {
        if (!cluster_open) return true;
        Buf b;
        const size_t c = ebml_master(b, 0x1F43B675);
        b.append(cluster);
        ebml_end(b, c);
        cluster.v.clear();
        cluster_open = false;
        return put(b);
    }

    bool mkv_block(uint32_t track, uint64_t ts_ms, bool key, const uint8_t* data, size_t n) {
        const bool new_cluster = !cluster_open || (track == 1 && key) || ts_ms < cluster_ts_ms ||
                                 ts_ms - cluster_ts_ms > 30000;
        if (new_cluster) {
            if (!flush_cluster()) return false;
            cluster_ts_ms = ts_ms;
            cluster_open = true;
            if (track == 1 && key) cues.push_back({ts_ms, file_pos - seg_data_pos});
            ebml_uint(cluster, 0xE7, ts_ms);
        }
        ebml_id(cluster, 0xA3);
        ebml_size(cluster, n + 4);
        cluster.u8(0x80 | track);
        const int16_t rel = int16_t(int64_t(ts_ms) - int64_t(cluster_ts_ms));
        cluster.u16(uint16_t(rel));
        cluster.u8(key ? 0x80 : 0x00);
        cluster.bytes(data, n);
        if (cluster.v.size() > (8u << 20)) return flush_cluster();  // bound memory per cluster
        return true;
    }

    bool close_mkv() {
        if (!tracks_written) {  // nothing recorded: still produce a valid (empty) file
            sps.clear();
        }
        bool ok = flush_cluster();
        if (!cues.empty()) {
            Buf b;
            const size_t cu = ebml_master(b, 0x1C53BB6B);
            for (const auto& c : cues) {
                const size_t cp = ebml_master(b, 0xBB);
                ebml_uint(b, 0xB3, c.ts_ms);
                const size_t tp = ebml_master(b, 0xB7);
                ebml_uint(b, 0xF7, 1);
                ebml_uint(b, 0xF1, c.pos);
                ebml_end(b, tp);
                ebml_end(b, cp);
            }
            ebml_end(b, cu);
            ok = put(b) && ok;
        }
        // Duration (ms, float) and Segment size
        const double dur_ms = double(last_video_pts_us) / 1000.0 + 33.0;
        uint64_t u;
        std::memcpy(&u, &dur_ms, 8);
        uint8_t be[8];
        for (int i = 0; i < 8; ++i) be[i] = uint8_t(u >> (56 - 8 * i));
        patch(duration_pos, be, 8);
        const uint64_t seg = file_pos - seg_data_pos;
        uint8_t sz[8];
        sz[0] = 0x01;
        for (int i = 0; i < 7; ++i) sz[1 + i] = uint8_t(seg >> (8 * (6 - i)));
        patch(seg_size_pos, sz, 8);
        return ok;
    }
};

IsoRecorder::IsoRecorder() = default;
IsoRecorder::~IsoRecorder() { close(); }

bool IsoRecorder::open(const IsoRecorderConfig& cfg) {
    std::lock_guard<std::mutex> lk(mu_);
    if (impl_) impl_->close();
    cfg_ = cfg;
    final_stats_ = IsoRecorderStats{};
    impl_ = std::make_unique<Impl>();
    impl_->cfg = cfg;
    if (!impl_->open()) {
        impl_.reset();
        return false;
    }
    return true;
}

bool IsoRecorder::write_video(const uint8_t* annexb, size_t size, bool is_keyframe, uint64_t pts_us) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!impl_ || !annexb || !size) return false;
    return impl_->write_video(annexb, size, is_keyframe, pts_us);
}

bool IsoRecorder::write_audio(const int16_t* pcm, uint32_t frames, uint64_t pts_us) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!impl_ || !pcm) return false;
    return impl_->write_audio(pcm, frames, pts_us);
}

bool IsoRecorder::close() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!impl_) return true;
    const bool ok = impl_->close();
    final_stats_ = impl_->st;
    impl_.reset();
    return ok;
}

bool IsoRecorder::is_open() const {
    std::lock_guard<std::mutex> lk(mu_);
    return impl_ != nullptr;
}

IsoRecorderStats IsoRecorder::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return impl_ ? impl_->st : final_stats_;
}

}  // namespace lv
