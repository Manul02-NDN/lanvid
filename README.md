# LanVid `v2.7.0`

[![CI Build & Test](https://github.com/Manul02-NDN/lanvid/actions/workflows/ci.yml/badge.svg)](https://github.com/Manul02-NDN/lanvid/actions/workflows/ci.yml)
[![Release Artifacts](https://github.com/Manul02-NDN/lanvid/actions/workflows/release.yml/badge.svg)](https://github.com/Manul02-NDN/lanvid/releases)
[![App Store — LanVid Camera](https://img.shields.io/badge/App%20Store-LanVid%20Camera-0D96F6?logo=apple&logoColor=white)](https://apps.apple.com/us/app/lanvid-camera/id6839672027)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)


**Ultra-Low Latency Multi-Codec LAN Video & Audio Streaming SDK (`LanVid DCT` / `H.264` / `H.265`, `C++20 / C ABI / Swift / Kotlin / Python`) and the `LanVid Camera` mobile app (iOS + Android).**

This repository contains only two things:

1. **LanVid Core SDK** — `lanvid_core` (static + shared C ABI library) built from `src/` and published through `sdk/`, with its unit tests in `tests/`.
2. **LanVid Camera** — the iPhone / Android broadcast camera app built on top of the SDK (`mobile/ios_camera/`, `android/`). [Download on the App Store](https://apps.apple.com/us/app/lanvid-camera/id6839672027).

---

## Key Features

- **Multi-Codec Architecture (`LanVid DCT` | `H.264` | `H.265 / HEVC`):**
  - **LanVid DCT (`I-Frame Only`):** Zero inter-frame dependencies (`no P/B frames`), zero GOP buffering latency, independent 16-row horizontal slices parallelized across all CPU cores with runtime-dispatched **x86_64 AVX2** and **ARM64 NEON** (`16-bit` Loeffler LLM 2D DCT/IDCT + fixed-point `BGRA/RGBA <-> YCbCr 4:2:0/4:2:2/4:4:4` + direct zero-RGB `YUV_420_888 / NV12 / NV21` input).
  - **Hardware Zero-Latency H.264 & H.265 (HEVC):** Native OS hardware encoders/decoders with zero B-frames (`B=0`), Annex-B `SPS/PPS/VPS + IDR` normalization, automatic receiver codec auto-detection from LVSP wire flags, dynamic AIMD bitrate scaling (`250 kbps – 100 Mbps`), and instant **PLI (Picture Loss Indication)** IDR keyframe recovery on unrecoverable packet loss:
    - **Windows:** Media Foundation MFT + shared `IMFDXGIDeviceManager` + `ID3D11VideoProcessor` (`BGRA/RGBA -> NV12` 4-slot GPU texture ring pool, zero CPU color conversion) + D3D11 DXVA HW decoding with compact NV12 readback & AVX2 color conversion.
    - **macOS / iOS:** Apple **VideoToolbox** (`VTCompressionSession` / `VTDecompressionSession`, `kVTCompressionPropertyKey_RealTime`, `AllowFrameReordering = false`).
    - **Android:** NDK **AMediaCodec** (`KEY_LOW_LATENCY`, `KEY_MAX_B_FRAMES = 0`).
    - **Linux:** VAAPI (`h264_vaapi` / `hevc_vaapi`) / NVENC / `libx264` / `libx265` zero-latency via `dlopen` FFmpeg (`libavcodec`).
- **Resilient UDP Transport (`LVSP v1`):** Raw MTU-safe UDP datagrams (`<= 1400 B`), hardware-accelerated CRC32C, **slice-level XOR FEC** (instant single-packet recovery with zero NACK round-trips), **PLI/IDR** fast recovery for inter-frame codecs, and **AIMD Adaptive Quality / Bitrate** driven by 100 ms receiver telemetry feedback.
- **64-Channel AoIP Bus (Dante / AES67-style):** up to 64 independent per-channel streams (PCM16 / PCM24 / F32, 48 / 96 kHz) on LVSP audio v2, a PTP-style Master Clock (min-RTT gated Kalman filter over PING/PONG), sample-accurate phase-coherent playout across all channels (`T_playout = T_master − latency`), shared ±100 ppm Hermite drift resampler, and a latency-scaled reliability ladder from **2 ms** (0.5 ms chunks, 1+1/2+1 XOR FEC) to **2000 ms** (interleaved FEC + multi-round NACK + duplication) — see [docs/PROTOCOL.md §7](docs/PROTOCOL.md).
- **Synchronized Low-Latency Audio + LAN Auto-Discovery:** 48 kHz Stereo `PCM16` / `Float32` in 2.5 ms or 5 ms UDP packets over the same socket, lock-free SPSC jitter buffer (~12 ms target) with adaptive 1-sample clock-drift compensation, WASAPI Loopback/Mic (Windows), native AAudio (Android), `AVAudioEngine` (Apple), and UDP broadcast LAN peer discovery (port `5099`).
- **LanVid Camera (iOS `mobile/ios_camera/` · Android `android/`):** Pro Cine HUD (manual shutter / ISO / WB / focus), live Tally (red `PGM` / green `PVW`), Duplex Intercom (IFB + PTT / Latch), Dual-Cam & iPhone Duo modes, low-latency `LanVid DCT` / `H.264` / `H.265` streaming to LanVid Video / Studio over local Wi-Fi.

### Multi-Codec Comparison (1080p60 Live UDP Loopback — Windows 11, Ryzen Zen 4 + AMD VCN 3.1 D3D11 HW)

| Codec Mode | Target / Avg Bitrate | Encode + TX (p50) | E2E UDP Latency (steady p50 / p99) | Delivered FPS | Recommended Use Case |
|---|---:|---:|---:|---:|---|
| **LanVid DCT (`Q=80`, AVX2)** | **76.1 Mbps** | **1.42 ms** | **2.00 ms / 2.34 ms** | **60.3 FPS** | 1GbE / 2.5GbE LAN, ultra-low-latency desktop & game capture |
| **H.264 Zero-Latency (D3D11 HW)** | **7.0 Mbps** (8M target) | **7.92–8.11 ms** *(down from 14.46 ms CPU-NV12)* | **11.51–11.97 ms / 13.39 ms** *(down from 19.33 ms)* | **60.3 FPS** | Wi-Fi, mobile clients, 10–15× lower bandwidth with HW offload |
| **H.264 + XOR FEC (`k=6`) + AIMD (1% loss)** | **5.3 Mbps** (adaptive) | **8.08 ms** | **11.29–11.90 ms / 13.27 ms** | **60.3 FPS** (`17/17` FEC, `0` drops) | Lossy Wi-Fi / congested LANs with zero stutter |
| **H.265 / HEVC Zero-Latency (D3D11 HW)** | **~8–9 Mbps** (8M target) | **6.21–6.30 ms** *(VCN raw ~3.1 ms)* | Hardware TX verified *(requires HEVC decoder MFT)* | **60.2 FPS** | High-efficiency 1080p/4K streaming over bandwidth-limited links |

> **Note on D3D11 GPU Pipeline:** Moving `BGRA/RGBA -> NV12` onto a 4-slot `ID3D11VideoProcessor` texture ring bound via `IMFDXGIDeviceManager` + `MFCreateDXGISurfaceBuffer`, paired with D3D11 DXVA hardware decoding and AVX2 NV12 readback (`~3.6 ms` total network + decode), cut H.264 `Encode+TX` by **~45%** (`14.46 ms -> 7.92 ms`) and steady-state loopback E2E latency by **~7.4 ms** (`19.33 ms -> 11.51 ms`), even with both sender and receiver sharing the same integrated 2-CU GPU on `127.0.0.1`. On separate LAN machines, encode and decode run on dedicated GPUs without contention.

### Slice-Codec (`LanVid DCT`) Peak Performance (AMD Zen 4, 16 HW Threads, AVX2)

| Workload | Encode (avg / p95) | Decode (avg / p95) | Throughput | E2E UDP Latency |
|---|---:|---:|---:|---:|
| **1080p (`1920×1080`, Q=75, AVX2)** | **0.22 ms** (0.26) | **0.18 ms** (0.20) | **4,471 FPS** | **1.76 ms p50 / 2.08 ms p99** |
| **4K UHD (`3840×2160`, Q=75, AVX2)** | **0.87 ms** (0.99) | **0.80 ms** (0.86) | **1,147 FPS** | **3.45 ms p50 / 3.98–4.84 ms p99** (with D3D11 Preview + XOR FEC) |

*Unit test suite: **43 / 43 passing** (including 64-channel AoIP phase sync to 0 samples across 1–40 ms paths, 64-ch lock-free shared-memory virtual audio bus roundtrip, 100 % recovery at 1000 ms under 15 % loss + 300 ms spike, live 2→500→5 ms latency switching, Android padded-stride `YUV_420_888` & 270° front-camera rotation parity `PSNR = 55.04 dB`, 4K AVX2 vs Scalar PSNR parity `ΔY = 0.03 dB`, XOR FEC single/double loss, AIMD feedback, lock-free audio drift, zero-copy SDK callbacks, UDP loopback A/V + LAN discovery, H.264 zero-latency roundtrip + PLI/IDR recovery, H.265 Annex-B VPS/SPS/PPS+IDR verification, and SDK H.264 auto-detect loopback).*

---

## Repository Structure

```text
lanvid/
├── CMakeLists.txt           # Root CMake build: lanvid_core (static + shared), lanvid_tests, SDK install / pkg-config
├── cmake/                   # Compiler flags & optimization presets
├── include/lanvid_c_api.h   # Internal C ABI header (used by the core, tests and the Android JNI bridge)
├── src/
│   ├── common/              # CPU SIMD dispatch, CRC32C, monotonic clock, Image buffers
│   ├── codec/               # Color (Scalar/AVX2/NEON), 2D LLM DCT/IDCT, Slice/Frame codec, HW H.264/H.265 backends
│   ├── parallel/            # Lock-free work pool for slice-level parallelism
│   ├── audio/               # 64-ch AoIP bus (MasterClock, AudioSyncMatrix, Tx/Rx FEC+NACK), jitter buffer, intercom
│   ├── net/                 # UDP sockets, Packetizer/FEC, Assembler, AIMD, Discovery, mDNS
│   ├── midi/                # MIDI sync engine & virtual MIDI ports
│   ├── grid/                # Grid L2 transport & DSP server engine
│   ├── video/               # Multicam receive manager, video bus, ISO recorder, playout buffer (lv_multicam_* API)
│   ├── app/                 # Test pattern, Y4M/PPM file source, telemetry stats (linked into the core)
│   ├── platform/            # OS capture / audio backends and headless preview stubs
│   └── core/                # C ABI implementation (lanvid_core.dll / liblanvid_core.so / .dylib)
├── sdk/                     # LanVidSDK v2.7.0: C headers, C++17 wrapper, SwiftPM / XCFramework, Android AAR, Python
├── tests/                   # SDK core unit & loopback integration tests (lanvid_tests)
├── mobile/ios_camera/       # LanVid Camera for iOS (XcodeGen project.yml, SwiftUI app, fastlane / App Store metadata)
├── android/                 # LanVid Camera for Android (Kotlin + Compose + Camera2 + JNI)
└── docs/                    # Protocol spec + LanVid Camera App Store pages (camera-privacy.html, support.html)
```

---

## Quick Build

### LanVid Core SDK + tests (Windows / Linux / macOS)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/lanvid_tests            # Windows: .\build\lanvid_tests.exe
python3 sdk/build_sdk.py --verify-client
```

### LanVid Camera — iOS

```bash
cd mobile/ios_camera
./scripts/build_core.sh         # build/LanVidCore.xcframework
xcodegen generate               # LanVidCamera.xcodeproj
xcodebuild -project LanVidCamera.xcodeproj -scheme LanVidCamera -configuration Release \
  -destination 'generic/platform=iOS' CODE_SIGNING_ALLOWED=NO build
```

### LanVid Camera — Android

```bash
(cd android && gradle --no-daemon exportUniversalApk)      # LanVid-Android-universal.apk
(cd sdk/android && gradle --no-daemon exportAar)           # SDK .aar
```

---

## Documentation

- **SDK Developer Guide (C++17, Swift iOS/macOS, Kotlin Android, Python):** [sdk/README.md](sdk/README.md)
- **Wire Protocol & Codec Specification (LVSP v1 / LVC):** [docs/PROTOCOL.md](docs/PROTOCOL.md)
- **LanVid Camera App Store setup:** [mobile/ios_camera/APPSTORE_SETUP.md](mobile/ios_camera/APPSTORE_SETUP.md)

## License

Licensed under the [MIT License](LICENSE). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
