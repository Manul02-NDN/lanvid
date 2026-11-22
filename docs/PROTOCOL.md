# LVSP v1 (LAN Video Slice Protocol) & LVC Codec

Ultra-low-latency intra-only LAN video + audio streaming protocol and slice-parallel SIMD codec in dependency-free C++20 (Windows, Linux, macOS).

## Codec (LVC)
- **Intra-only (I-frame only):** zero inter-frame coding dependencies.
- **Independent slices:** frame partitioned into horizontal strips (default 16 luma rows -> 68 slices at 1080p, 135 slices at 4K); DC DPCM resets at every slice boundary; each slice carries a 16-byte self-describing header.
- **Compression pipeline:**
  - RGBA8/BGRA8 <-> planar YCbCr (BT.601/BT.709 full-range; 4:2:0, 4:2:2, or 4:4:4) with runtime-dispatched **AVX2** (x86_64) and **NEON** (ARM64 / Apple Silicon) fixed-point vectorization.
  - 8×8 2D DCT/IDCT: scalar AAN float32 fallback + **AVX2** dual-block `int16_t` (`__m256i`) and **NEON** `int16x8_t` Loeffler (LLM) DCT/IDCT with in-register 8×8 transpose and fused quantization — 100% bitstream-compatible across x86_64 and ARM64.
  - IJG-scaled JPEG Annex K quantization (Quality 1–100) + RLE (EOB/ZRL) + canonical static Annex K Huffman (9-bit fast decode LUT, no byte stuffing).

## Binary Wire Format (Little-Endian, Explicit Serialization)

### 1. Common Packet Header — 28 bytes (4-byte aligned)
| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 2 | `magic` | `0x4C 0x56` (`"LV"`) |
| 2 | 1 | `ver_type` | high nibble `version=1`, low nibble `type`: `1=VIDEO_SLICE, 2=PING, 3=AUDIO, 4=FEEDBACK, 5=PONG` |
| 3 | 1 | `flags` | bit0 `HAS_CRC` (`0x01`), bit1 `LAST_FRAG` (`0x02`), bit2 `LAST_SLICE_OF_FRAME` (`0x04`), bit3 `FEC_PARITY` (`0x08`) |
| 4 | 4 | `frame_id` | u32 monotonic video `frame_id` or `audio_seq_id` (RFC 1982 wrap comparison) |
| 8 | 4 | `timestamp_us` | u32 shared monotonic clock (`steady_clock`/QPC) for A/V sync & E2E latency |
| 12 | 2 | `slice_id` | u16 (`0 .. total_slices-1`) |
| 14 | 2 | `total_slices` | u16 |
| 16 | 2 | `frag_index` | u16: data packets `0 .. K-1`; FEC parity packets `K + group_index` |
| 18 | 2 | `frag_count` | u16: `K` (number of data fragments in the slice) |
| 20 | 2 | `payload_size` | u16 bytes following header |
| 22 | 2 | `seq` | u16 stream-wide video sequence number (independent from audio) |
| 24 | 4 | `crc32c` | CRC32C (Castagnoli) over header `[0..23]` + payload (0 if `!HAS_CRC`) |

### 2. Slice Header — 16 bytes (start of reassembled video slice, 4-byte aligned)
| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 2 | `width` | Frame width in pixels |
| 2 | 2 | `height` | Frame height in pixels |
| 4 | 2 | `slice_y0` | First luma row of this slice |
| 6 | 2 | `slice_h` | Height of this slice in luma rows |
| 8 | 1 | `chroma` | `0=4:2:0, 1=4:2:2, 2=4:4:4` |
| 9 | 1 | `quality` | `1..100` (active encoder quality; shown in receiver HUD) |
| 10 | 1 | `color_matrix` | `0=BT.601 full, 1=BT.709 full` |
| 11 | 1 | `entropy_mode` | `0=static Annex K` |
| 12 | 4 | `bitstream_bytes` | Exact length of entropy bitstream following this header |

### 3. Audio Payload (`type=AUDIO (3)`) — 8-byte `AudioHeader` + PCM samples
Each chunk is 2.5 ms (120 frames = 480 B) or 5 ms (240 frames = 960 B) at 48 kHz Stereo PCM16 -> total datagram 516–996 bytes (zero fragmentation).

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | `sample_rate` | u32 LE (default `48000`) |
| 4 | 1 | `channels` | u8 (`1` or `2`) |
| 5 | 1 | `sample_format` | u8 (`0=PCM16`, `1=F32`) |
| 6 | 2 | `num_frames` | u16 LE samples per channel |
| 8 | `N` | `pcm_data` | Interleaved sample payload (`num_frames * channels * bytes_per_sample`) |

### 4. Slice-Level XOR FEC Parity Payload (`type=VIDEO_SLICE`, `flags & FEC_PARITY`)
8-byte `FecHeader` (`group_start_frag`, `group_data_count`, `xor_payload_len`, `reserved`) followed by byte-wise XOR parity (`max_len` bytes). When FEC is enabled (`--fec 1` or `--fec-group N`), data payload is capped at `MTU - 28 - 8` so parity datagrams remain `<= MTU`.

### 5. Telemetry Feedback Payload (`type=FEEDBACK (4)`, Receiver -> Sender every 100 ms)
16-byte `FeedbackPayload` (`last_completed_frame_id`, `packet_loss_permille`, `fec_recovered_permille`, `jitter_us`, `decode_time_us`) driving AIMD congestion control (`--adaptive-quality`).

## Platform Backends & Build

| Feature | Windows | Linux | macOS |
|---|---|---|---|
| UDP Transport | Winsock2 | POSIX + `sendmmsg`/`recvmmsg` | POSIX BSD sockets |
| SIMD | AVX2 (runtime CPUID) | AVX2 (x86_64) / NEON (ARM64) | AVX2 (x86_64) / NEON (Apple Silicon) |
| Preview Window | Direct3D 11 Flip-Model + GDI fallback | Optional SDL2 (auto-detected) | Optional SDL2 (auto-detected) |
| Capture / Audio | DXGI/GDI screen, MF webcam, WASAPI audio | Pattern + Y4M/PPM/raw file | Pattern + Y4M/PPM/raw file |

```bash
# Windows / Linux / macOS
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Unit tests (27 tests: DCT/AVX2 4K parity, FEC, AIMD, Audio jitter buffer, Y4M/PPM, A/V loopback)
./build/lanvid_tests

# Benchmark 1080p & 4K60 (scalar vs AVX2/NEON)
./build/bench_codec

# Windows 4K60 desktop + system audio (WASAPI loopback) -> D3D11 preview + WASAPI playback
./build/receiver --port 5000 --preview
./build/sender --source screen --monitor 0 --audio loopback --host 127.0.0.1 --port 5000 --fps 60 --fec-group 6 --adaptive-quality

# Linux / macOS streaming from Y4M / PPM / pattern
./build/receiver --port 5000 --preview
./build/sender --source file --input sample.y4m --host 127.0.0.1 --port 5000
```

### Preview Window Hotkeys (`receiver --preview`)
- `Esc` / `Q` — close preview and exit
- `F` / `F11` / double-click — toggle borderless fullscreen
- `C` — live toggle concealment mode (`prev` <-> `gray`)
- `S` — save current displayed frame as `snapshot_XXXX.ppm`
