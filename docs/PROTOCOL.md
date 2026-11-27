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
| 2 | 1 | `ver_type` | high nibble `version=1`, low nibble `type`: `1=VIDEO_SLICE, 2=PING, 3=AUDIO, 4=FEEDBACK, 5=PONG, 6=AUDIO2, 7=AUDIO_NACK, 8=MIDI_FRAME, 9=CONTROL_SYNC, 10=TALLY_STATE, 11=INTERCOM_AUDIO, 12=CAMERA_CONTROL, 13=VIDEO_CONFIG_SYNC, 14=VIDEO_KEYFRAME_REQ` |
| 3 | 1 | `flags` | bit0 `HAS_CRC` (`0x01`), bit1 `LAST_FRAG` (`0x02`), bit2 `LAST_SLICE_OF_FRAME` (`0x04`), bit3 `FEC_PARITY` (`0x08`), bit4 `KEYFRAME` (`0x10`, IDR), bit5 `CODEC_CONFIG` (`0x20`, SPS/PPS/VPS present), bits 6–7 `codec_id` (`0xC0`: `0=LANVID_DCT`, `1=H264`, `2=H265`) |
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

### 5. Telemetry Feedback & PLI Payload (`type=FEEDBACK (4)`, Receiver -> Sender every 100 ms or immediately on PLI)
16-byte `FeedbackPayload` (`last_completed_frame_id`, `packet_loss_permille | (request_idr << 15)`, `fec_recovered_permille`, `jitter_us`, `decode_time_us`) driving:
- **AIMD congestion control (`--adaptive-quality`):** adjusts JPEG Q (`35..85`) for `LANVID_DCT`, or dynamically updates `target_bitrate_kbps` (`1500..50000 kbps`) without recreating the encoder for `H.264` / `H.265`.
- **Instant PLI / IDR Recovery (`request_idr = 1`, bit 15 of offset 4):** when an H.264/H.265 Access Unit cannot be recovered even after XOR FEC (or a late-joining receiver enters mid-GOP), the receiver sets `awaiting_idr = true`, drops dependent P-frames to avoid macroblock corruption, and transmits a `FEEDBACK` packet with `request_idr = 1` immediately. The sender forces the very next encoded frame to be an IDR keyframe prepended with `[VPS][SPS][PPS]`.

### 6. H.264 (AVC) & H.265 (HEVC) Zero-Latency Annex-B Framing (`codec_id = 1 | 2`)
- **Zero B-frames (`B = 0`, Low-Latency mode):** every encoded Access Unit is transmitted immediately upon completion with `slice_id = 0`, `total_slices = 1`, fragmented under `MTU` and optionally protected by slice-level XOR FEC (`--fec-group N`).
- **Self-contained IDRs:** `[SPS][PPS]` (H.264) or `[VPS][SPS][PPS]` (H.265) in Annex-B start-code format (`00 00 00 01`) are always prepended to every IDR frame (`flags |= KEYFRAME | CODEC_CONFIG`), allowing late-joining receivers and post-PLI streams to decode cleanly on the very first keyframe.
- **Auto-detection:** the receiver inspects bits 6–7 of `PacketHeader::flags` on every frame and switches transparently between `LANVID_DCT`, `H.264`, and `H.265` on the same UDP port.
- **Hardware Acceleration Backends:**
  - **Windows:** Windows Media Foundation (`IMFTransform` / `MFTEnumEx` HW MFT -> Sync MFT fallback; zero external DLLs).
  - **macOS / iOS:** Apple VideoToolbox (`VTCompressionSession` / `VTDecompressionSession`, `RealTime=true`, `AllowFrameReordering=false`).
  - **Android:** NDK `AMediaCodec` (`video/avc`, `video/hevc`, `KEY_LOW_LATENCY=1`).
  - **Linux:** Optional FFmpeg `libavcodec` (`h264_nvenc` / `hevc_nvenc` / `h264_vaapi` / `hevc_vaapi` -> `libx264` / `libx265` `ultrafast`/`zerolatency`).

### 7. 64-Channel AoIP Audio v2 (`type=AUDIO2 (6)`, `type=AUDIO_NACK (7)`)
Independent per-channel streams (up to 64) aligned to one **Master Clock** for sample-accurate,
comb-filter-free playout. The legacy stereo `AUDIO (3)` path is unchanged and still supported.

**LVSP header usage:** `frame_id` = per-stream packet sequence (u32, RFC 1982), `slice_id` = `stream_id`,
`timestamp_us` = sender wallclock.

**`AudioHeader2` — 24 bytes** (the requested fields sum to 24, not 16):

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 2 | `stream_id` | 0..63 independent transport stream (own seq / FEC / NACK state) |
| 2 | 2 | `channel_index` | 0..63 destination slot on the receiver's 64-channel bus |
| 4 | 4 | `sample_rate` | 48000 / 96000 |
| 8 | 2 | `num_samples` | samples of this single channel in the packet |
| 10 | 1 | `sample_format` | `1=PCM16`, `2=PCM24_PACKED`, `3=F32` (C ABI `LV_AudioFormat` keeps `PCM16=0, F32=1, PCM24=2`) |
| 11 | 1 | `flags` | `0x01 FEC_PARITY`, `0x02 RETRANSMIT`, `0x04 DUPLICATE` |
| 12 | 8 | `media_timestamp_samples` | master-clock sample position of the first sample |
| 20 | 4 | `wallclock_us` | sender steady clock at transmit (jitter / RTT) |

Data packets carry `num_samples * bytes_per_sample` bytes after the header.
**Parity packets** (`FEC_PARITY`) carry a 4-byte `AudioFecInfo` (`group_size k`, `interleave d`, `u16 reserved`)
plus the XOR of the members' sample bytes. Members are `seq = frame_id + j*d`, `j = 0..k-1`; `media_timestamp_samples`
is the first member's position, so a single missing member is rebuilt bit-exactly with its exact timestamp.

**`AUDIO_NACK` payload — 16 bytes:** `stream_id u16 | reserved u16 | base_seq u32 | mask u64` (bit `i` set ⇒
`base_seq + i` missing). The sender answers from a per-stream history ring with `RETRANSMIT` set.

**Master Clock (extended PONG):** the sender's master timeline is `master_us = steady_us - epoch`. PONG carries a
16-byte tail after the 12-byte `PingPongPayload`: `t2_master_us u64 | t3_master_us u64`. The receiver computes
`θ = ((t2−t1)+(t3−t4))/2`, `δ = (t4−t1)−(t3−t2)`, keeps only exchanges within 150 µs (or 25 %) of the 64-sample
minimum RTT, and runs a 2-state Kalman filter (offset + skew) with noise proportional to the excess delay.
Old receivers ignore the tail; old senders simply don't send it (the receiver then free-runs on newest arrivals).

**Phase-coherent playout (`AudioSyncMatrix`):** 64 lock-free sample-addressed float32 rings (2.5 s each;
allocated per active channel; ~33 MB float + 17 MB tags at 64 ch / 48 kHz). All channels are rendered from one
cursor `T_playout = T_master_now − TargetLatencySamples`, so different network paths cannot skew channels.
Clock drift between master and the local audio device is absorbed by a single shared ±100 ppm 4-point Hermite
micro-resampler (exact copy when the phase error is within the 1.5-sample deadband). Latency changes are applied
live with a 96-sample crossfade.

**Reliability profiles (`target_latency_ms` 2..2000):**

| Latency | Mode | Chunk | XOR FEC | ARQ |
|---|---|---|---|---|
| 2 – 10 ms | Ultra-Low | 0.5 ms (≤ 4 ms) / 1 ms | 1+1 or 2+1 | none |
| 10 – 100 ms | Balanced / Live | 2.5 ms (≤ 40 ms) / 5 ms | up to 4+1 | 1 fast NACK |
| 100 – 2000 ms | High-Reliability | 10 ms (≤ 500 ms) / 20 ms | up to 4+1, interleave ≤ 4 | 5..12 NACK rounds + duplication when NACK rate > 5 % |

Rules: an FEC group never spans more than `latency/3`; chunks are clamped so packet + header stay within the MTU
(e.g. 20 ms PCM16 at 48 kHz → 672 samples = 14 ms per packet at MTU 1400); NACK first wait = `clamp(latency·0.1, 2, 100) ms`,
retry spacing = `latency·0.6 / attempts`; a hole is abandoned once its samples have passed the playout cursor.

### 8. Stream epoch, MIDI2 transport and QoS (v2.4.x hardening)

* **Session epoch:** every sender picks a random non-zero 16-bit epoch. AUDIO2 packets carry it in
  `PacketHeader.total_slices` (unused for audio), MIDI2 frames in `MidiFrameHeader.stream_id`. When
  the receiver sees a new epoch (sender restart) it resets FEC groups, sequence windows and NACK
  schedule, the `event_seq` de-duplicator and queued MIDI, the MasterClock (immediate re-ping), and
  re-anchors the AudioSyncMatrix with a 96-sample crossfade into silence; every note still held is
  released with a synthesized Note Off. Late packets of the previous epoch are dropped. Epoch 0 =
  legacy sender (no epoch handling).
* **MIDI2 on the live stream:** `UdpSender::push_midi` / `lv_sender_push_midi` / `lv_duplex_send_midi`
  send `kMidiFrame` packets on the audio socket, stamped with the sender MasterClock and repeated via
  the 16-event redundancy ring plus 100 ms heartbeats (2 s after the last event). The receiver
  dispatches events from inside `pop_audio_multichannel_*` for the block that contains them
  (`block_offset_frames` = sample position), or on the network thread on the MasterClock playout
  estimate when no audio is pulled.
* **QoS:** all LVSP sockets request DSCP 46 / EF (`IP_TOS 0xB8`, Wi-Fi WMM AC_VO; on Apple also
  `SO_NET_SERVICE_TYPE = NET_SERVICE_TYPE_VO`) and use >= 2 MB send/receive buffers.
* **Real-time threads:** mixer, SHM pump and network RX threads use Mach
  `THREAD_TIME_CONSTRAINT_POLICY` (macOS), MMCSS "Pro Audio" + `timeBeginPeriod(1)` (Windows) or
  `SCHED_FIFO` when permitted (Linux).

### 9. Jitter-buffer latency mode: Fixed vs Adaptive (receiver side, no wire change)

The playout delay of the 64-channel `AudioSyncMatrix` runs in one of two modes, switchable live
without restarting the stream:

| Mode | Use | Behaviour |
|---|---|---|
| **Fixed (Ethernet)** | wired LAN, live monitoring | delay held at exactly `latency_ms`; only +/-100 ppm device-clock drift is tracked. Late packets are dropped, the buffer never floats. |
| **Adaptive (Wi-Fi)** — default | Wi-Fi 6, shared links | delay grows within `[latency_ms .. 200 ms]` to `max(arrival-lag envelope + render block + 2 ms, 3 x RFC 3550 jitter + 2 ms)`. Small growth steps use +/-0.05 % micro-resampling, steps > 10 ms one crossfaded re-anchor; after a 3 s hold the delay glides back at 0.4 ms/s through +/-0.05 % micro-resampling. The resampling ratio is hard-limited to 0.9995 .. 1.0005 in both modes. |

The arrival lag is `MasterClock position at arrival - end of the block's samples`, measured per directly
received AUDIO2 packet (envelope: instant attack, 4 s release). Steps larger than 40 ms, a switch from a
grown Adaptive buffer to Fixed and a consumer stall all re-anchor once with a 96-sample crossfade.

`current_effective_latency_ms()` reports the measured delay (`MasterClock now - playout cursor`) for telemetry displays.

* C ABI: `LvReceiverConfig.adaptive_jitter`, `LV_DuplexConfig.adaptive_jitter` (both default 1),
  `lv_receiver_set_adaptive_jitter()`, `lv_duplex_set_adaptive_jitter()`, `lv_receiver_get_effective_latency_ms()`,
  `LvReceiverStats` / `LV_DuplexStats` `.effective_latency_ms`, `.effective_target_ms`, `.adaptive_jitter` (appended fields).

## Platform Backends & Build

| Feature | Windows | Linux | macOS |
|---|---|---|---|
| UDP Transport | Winsock2 | POSIX + `sendmmsg`/`recvmmsg` | POSIX BSD sockets |
| SIMD | AVX2 (runtime CPUID) | AVX2 (x86_64) / NEON (ARM64) | AVX2 (x86_64) / NEON (Apple Silicon) |
| HW Video Codecs | Media Foundation (H.264/H.265) | FFmpeg `libavcodec` (NVENC/VAAPI/x264/x265) | VideoToolbox (H.264/H.265, zero-copy `CVPixelBuffer`) |
| Capture / Audio | DXGI/GDI screen, MF webcam, WASAPI audio | Pattern + Y4M/PPM/raw file | CoreAudio / CoreMIDI + Pattern + Y4M/PPM/raw file |

```bash
# Windows / Linux / macOS — build Core SDK (lanvid_core static & shared) + unit tests
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run full SDK core test suite (DCT/AVX2/NEON parity, FEC, AIMD, 64-ch AoIP sync, MIDI, L2 Grid, Multi-Cam/ISO/Playout)
./build/lanvid_tests

# Package cross-platform SDK distribution + verify external CMake consumer
python3 sdk/build_sdk.py --verify-client
```
