# LanVid Video Protocol — LVSP v1 & LVC Codec (v3.0.0)

Ultra-low-latency LAN video + 2-channel stereo audio streaming protocol, slice-parallel SIMD codec (`LVC`), hardware H.264/H.265 transport, Multi-Cam reception, ISO recording, virtual camera VideoBus, and camera Tally/Intercom.

> **Protocol Separation:** This specification covers strictly the **LanVid Video SDK (`liblanvid_core`, `lanvid/lanvid_sdk.h`)**. For 64-channel studio AoIP, MIDI 2.0, Personal Monitor Bus, DAW SHM Audio Bus, and Layer-2 Grid audio, see [docs/PROTOCOL_CROSSLINK.md](PROTOCOL_CROSSLINK.md).

---

## 1. Video Codecs
- **LanVid DCT (`LVC`, I-frame only, `codec_id = 0`):** Zero inter-frame dependencies, independent 16-row horizontal slices (68 slices @ 1080p, 135 @ 4K), 16-bit Loeffler LLM 2D DCT/IDCT with runtime-dispatched **x86_64 AVX2** and **ARM64 NEON**, fixed-point `BGRA/RGBA <-> YCbCr 4:2:0/4:2:2/4:4:4`, direct zero-RGB `YUV_420_888 / NV12 / NV21` input, Annex-K Huffman.
- **Zero-Latency H.264 (`codec_id = 1`) & H.265/HEVC (`codec_id = 2`):** `B = 0`, Annex-B `[VPS][SPS][PPS]` prepended to every IDR keyframe (`KEYFRAME | CODEC_CONFIG`), receiver codec auto-detection, dynamic AIMD bitrate scaling (`250 kbps – 150 Mbps`), and instant **PLI/IDR** keyframe recovery:
  - **Windows:** Media Foundation + `IMFDXGIDeviceManager` + `ID3D11VideoProcessor` GPU `BGRA -> NV12`
  - **macOS / iOS:** Apple VideoToolbox (zero-copy IOSurface `CVPixelBuffer`)
  - **Android:** NDK `AMediaCodec` (`KEY_LOW_LATENCY = 1`)
  - **Linux:** FFmpeg `libavcodec` (NVENC / VAAPI / `libx264` / `libx265`)

---

## 2. Wire Format (Little-Endian, Magic `"LV"`)

### Common Packet Header — 28 bytes
| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 2 | `magic` | `0x4C 0x56` (`"LV"`) |
| 2 | 1 | `ver_type` | high nibble `version=1`, low nibble `type` |
| 3 | 1 | `flags` | bit0 `HAS_CRC`, bit1 `LAST_FRAG`, bit2 `LAST_SLICE_OF_FRAME`, bit3 `FEC_PARITY`, bit4 `KEYFRAME`, bit5 `CODEC_CONFIG`, bits 6–7 `codec_id` |
| 4 | 4 | `frame_id` | u32 monotonic video `frame_id` or stereo audio `seq_id` |
| 8 | 4 | `timestamp_us` | u32 monotonic microsecond timestamp |
| 12 | 2 | `slice_id` | u16 (`0 .. total_slices - 1`) |
| 14 | 2 | `total_slices` | u16 |
| 16 | 2 | `frag_index` | u16: data `0..K-1`, XOR FEC parity `K + group_index` |
| 18 | 2 | `frag_count` | u16: `K` data fragments |
| 20 | 2 | `payload_size` | u16 bytes following header |
| 22 | 2 | `seq` | u16 video sequence counter |
| 24 | 4 | `crc32c` | Castagnoli CRC32C over header `[0..23]` + payload |

### Allowed LanVid Packet Types
| Value | Name | Direction | Purpose |
|---:|---|---|---|
| `1` | `kVideoSlice` | TX -> RX | Video slice / Annex-B AU fragment or XOR FEC parity |
| `2` | `kPing` | RX -> TX | RTT probe (`12-byte PingPongPayload`) |
| `3` | `kAudio` | TX -> RX | **1–2 channel stereo audio** (`8-byte AudioHeader` + PCM16/F32) |
| `4` | `kFeedback` | RX -> TX | AIMD quality/bitrate feedback + PLI (`request_idr` bit 15) |
| `5` | `kPong` | TX -> RX | RTT echo (`12-byte PingPongPayload`) |
| `10` | `kTallyState` | Studio <-> Cam | Camera Standby / Preview / Program ON AIR + battery/temp telemetry (port `5022`) |
| `11` | `kIntercomAudio` | Studio <-> Cam | Duplex Director <-> Camera Operator VOIP intercom (Opus / PCM16) |
| `12` | `kCameraControl` | Studio -> Cam | Remote lens, focus, zoom, quality preset |
| `13` | `kVideoConfigSync` | Studio <-> Cam | Bidirectional video config, camera number badge (`CAM 1..16`), IDR request |

Packets with non-`"LV"` magic (such as CrossLink `"CL"`) or unsupported packet types are rejected at parse time.

---

## 3. Stereo Audio Payload (`type = kAudio (3)`)
Carries **1 (mono) or 2 (stereo)** interleaved channels alongside video on the same UDP socket (`LV_AUDIO_MAX_CHANNELS = 2`; `> 2` channels are rejected with `LV_ERR_INVALID_ARG`).

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | `sample_rate` | u32 LE (default `48000`) |
| 4 | 1 | `channels` | u8 (`1` or `2` strictly enforced) |
| 5 | 1 | `sample_format` | `0 = PCM16`, `1 = Float32` |
| 6 | 2 | `num_frames` | u16 LE frames per channel (typically `120` = 2.5 ms or `240` = 5 ms) |
| 8 | `N` | `pcm_data` | Interleaved sample payload |

The receiver feeds decoded stereo samples into a lock-free SPSC `AudioJitterBuffer` (live-adjustable target `5..60 ms`, adaptive 1-sample drift compensation).

---

## 4. Network Ports & Discovery
- **Video + Stereo Audio UDP:** default `5000` (Multi-Cam demux base `5006`)
- **Video LAN Auto-Discovery:** UDP broadcast `5099` (`LVDS_PRB` / `LVDS_RSP`) and studio beacon `5023`
- **Tally & Duplex Intercom:** UDP `5022`
- **QoS:** DSCP 46 EF (`IP_TOS 0xB8`) / `NET_SERVICE_TYPE_VO`, optional Dual-NIC 1+1 failover.
