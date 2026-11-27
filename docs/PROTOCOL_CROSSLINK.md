# CrossLink Audio Protocol — CLAP v1 (v1.0.0)

Ultra-low-latency 64-channel studio Audio-over-IP (AoIP), MasterClock synchronization, Full-Duplex 64×64 DAW Shared-Memory Audio Bus, Channel Patchbay, MIDI 2.0 over IP, ControlSync, Personal Monitor Bus, and Layer-2 Grid DSP protocol.

> **Protocol Separation:** This specification covers strictly the **CrossLink Audio SDK (`libcrosslink_core`, `crosslink/crosslink_sdk.h`)**. It contains zero video codecs, video framers, or camera pipelines. For LAN video + 2-channel stereo streaming, see [docs/PROTOCOL_LANVID.md](PROTOCOL_LANVID.md).

---

## 1. Binary Wire Format (Little-Endian, Magic `"CL"`)

### Common Packet Header — 24 bytes (4-byte aligned)
| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 2 | `magic` | `0x43 0x4C` (`"CL"`) |
| 2 | 1 | `ver_type` | high nibble `version = 1`, low nibble `type` |
| 3 | 1 | `flags` | bit0 `HAS_CRC` (`0x01`) |
| 4 | 4 | `seq_id` | u32 per-stream sequence number (RFC 1982 wrap comparison) |
| 8 | 4 | `timestamp_us` | u32 sender monotonic microsecond clock |
| 12 | 2 | `stream_id` | u16 audio channel/stream ID (`0..63`) |
| 14 | 2 | `stream_epoch` | u16 random non-zero sender session epoch (detects live restarts) |
| 16 | 2 | `payload_size` | u16 bytes following header |
| 18 | 2 | `seq16` | u16 stream-wide 16-bit sequence counter |
| 20 | 4 | `crc32c` | Castagnoli CRC32C over header `[0..19]` + payload (0 if `!HAS_CRC`) |

### CrossLink Packet Types
| Value | Name | Purpose |
|---:|---|---|
| `1` | `kAudioFrame` | 64-channel phase-coherent AoIP channel frame or XOR FEC parity |
| `2` | `kAudioNack` | Selective 64-packet bitmask NACK (`stream_id`, `base_seq`, `mask64`) |
| `3` | `kPing` | MasterClock PTP-style sync probe (`12-byte PingPongPayload`) |
| `4` | `kPong` | Extended MasterClock echo (`12-byte PingPongPayload` + `t2_master_us`, `t3_master_us`) |
| `5` | `kMidiFrame` | Sample-accurate MIDI 2.0 frame with 16-event redundancy ring & heartbeats |
| `6` | `kControlSync` | Bidirectional 3× redundant studio audio control sync (channels, bitrate, latency, codec, gain) |
| `7` | `kMonitorAudioFlow` | Personal Monitor Bus multi-channel flow & DSP remote control |

Packets with non-`"CL"` magic (such as LanVid `"LV"`) are rejected as `kBadMagic`.

---

## 2. 64-Channel Phase-Coherent AoIP (`kAudioFrame`, `kAudioNack`)
- **24-byte `AudioHeader2`:** `stream_id` (0..63), `channel_index` (0..63), `sample_rate` (48000 / 96000), `num_samples`, `sample_format` (`PCM16`, `PCM24_PACKED`, `F32`, `Opus CELT`, `IMA-ADPCM 4/5-bit`), `flags` (`FEC_PARITY`, `RETRANSMIT`, `DUPLICATE`), `media_timestamp_samples` (64-bit MasterClock sample position), `wallclock_us`.
- **MasterClock (`kPing` / `kPong`):** Min-RTT gated 2-state Kalman filter (offset + clock skew ppm) locking all 64 channels to one timeline (`T_playout = T_master_now - TargetLatencySamples`) with a shared ±100 ppm 4-point Hermite micro-resampler.
- **Latency-Scaled Reliability Ladder (`2..2000 ms` live switchable):**
  - `2–10 ms` (Ultra-Low): 0.5–1 ms chunks, 1+1 / 2+1 XOR FEC
  - `10–100 ms` (Balanced / Live): 2.5–5 ms chunks, up to 4+1 XOR FEC + fast NACK
  - `100–2000 ms` (High-Reliability): 10–20 ms chunks, interleaved XOR FEC + multi-round NACK + proactive duplication
- **Wi-Fi 1+1 Staggered Redundancy & Dual-NIC Failover:** Optional secondary NIC socket with instantaneous failover and zero-duplicate receiver filtering.
- **Latency Modes:** **Adaptive (Wi-Fi)** (default, smoothly expands/contracts within `[target .. 200 ms]` via ±0.05% micro-resampling) and **Fixed (Ethernet)** (exact deterministic delay).

---

## 3. MIDI 2.0, Personal Monitor Bus, DAW Bus & Layer-2 Grid
- **MIDI 2.0 over IP (`kMidiFrame`):** Sample-accurate `MidiDejitterQueue` locked to `MasterClock` (or `<= 1.5 ms` ultra-low-latency in MIDI-only mode), automatic hung-note panic release on `stream_epoch` changes, native CoreMIDI (`"CrossLink MIDI"`) / WinMM virtual MIDI ports.
- **Full-Duplex 64×64 DAW Audio Bus:** Lock-free shared-memory segment `CrossLinkAudioBus_v1` (magic `"CLAB"` = `0x42414C43`) connecting OS virtual audio drivers (CoreAudio HAL / ASIO / WASAPI) with a 64×64 `ChannelPatchbay` (per-channel routing, mute/solo).
- **Personal Monitor Bus:** 64-ch -> stereo personal IEM monitor mixer with per-musician HPF, 3-band EQ, compressor, limiter, Opus/Lossless streaming, `"CLKA"` keep-alive, and `"CL_MON_DISC_REQ"` / `"CL_MON_DISC_RESP|"` LAN discovery.
- **CrossLink Layer-2 Grid (`cl::grid`):** Raw Ethernet EtherType `0x88B6` (`ClL2Header`) sub-millisecond DSP offload (`GridNode` <-> `DspServer`) with UDP fallback.
- **Audio Node LAN Discovery:** UDP port `5021` (`"CLDS_PRB|"` / `"CLDS_RSP|"`).
- **QoS & RT Threads:** DSCP 46 EF (`IP_TOS 0xB8`), Mach `THREAD_TIME_CONSTRAINT_POLICY` / MMCSS `"Pro Audio"` / POSIX `SCHED_FIFO`.
