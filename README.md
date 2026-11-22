# LanVid `v2.4.0`

[![CI Build & Test](https://github.com/Manul02-NDN/lanvid/actions/workflows/ci.yml/badge.svg)](https://github.com/Manul02-NDN/lanvid/actions/workflows/ci.yml)
[![Release Artifacts](https://github.com/Manul02-NDN/lanvid/actions/workflows/release.yml/badge.svg)](https://github.com/Manul02-NDN/lanvid/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**Ultra-Low Latency Multi-Codec LAN Video & Audio Streaming Protocol (`LanVid DCT` / `H.264` / `H.265`), Cross-Platform Apps (`LanVid Studio`) & Multi-Platform SDK (`C++20 / C ABI / Swift / Kotlin / Python`).**

LanVid is engineered from scratch in C++20 for sub-frame **4K60** local network streaming (**< 1 ms** DCT codec latency, **< 4–6 ms** glass-to-glass LAN latency) and bandwidth-efficient **Zero-Latency Hardware H.264 / H.265 (HEVC)** streaming over Wi-Fi and constrained networks with instant **XOR FEC + PLI/IDR** packet-loss recovery and synchronized low-latency audio.

---

## Key Features & Measured Benchmarks

- **Multi-Codec Architecture (`LanVid DCT` | `H.264` | `H.265 / HEVC`):**
  - **LanVid DCT (`I-Frame Only`):** Zero inter-frame dependencies (`no P/B frames`), zero GOP buffering latency, independent 16-row horizontal slices parallelized across all CPU cores with runtime-dispatched **x86_64 AVX2** and **ARM64 NEON** (`16-bit` Loeffler LLM 2D DCT/IDCT + fixed-point `BGRA/RGBA <-> YCbCr 4:2:0/4:2:2/4:4:4` + direct zero-RGB `YUV_420_888 / NV12 / NV21` input).
  - **Hardware Zero-Latency H.264 & H.265 (HEVC):** Native OS hardware encoders/decoders with zero B-frames (`B=0`), Annex-B `SPS/PPS/VPS + IDR` normalization, automatic receiver codec auto-detection from LVSP wire flags, dynamic AIMD bitrate scaling (`250 kbps – 100 Mbps`), and instant **PLI (Picture Loss Indication)** IDR keyframe recovery on unrecoverable packet loss:
    - **Windows:** Media Foundation MFT + shared `IMFDXGIDeviceManager` + `ID3D11VideoProcessor` (`BGRA/RGBA -> NV12` 4-slot GPU texture ring pool, zero CPU color conversion) + D3D11 DXVA HW decoding with compact NV12 readback & AVX2 color conversion.
    - **macOS / iOS:** Apple **VideoToolbox** (`VTCompressionSession` / `VTDecompressionSession`, `kVTCompressionPropertyKey_RealTime`, `AllowFrameReordering = false`).
    - **Android:** NDK **AMediaCodec** (`KEY_LOW_LATENCY`, `KEY_MAX_B_FRAMES = 0`).
    - **Linux:** VAAPI (`h264_vaapi` / `hevc_vaapi`) / NVENC / `libx264` / `libx265` zero-latency via `dlopen` FFmpeg (`libavcodec`).
- **Resilient UDP Transport (`LVSP v1`):** Raw MTU-safe UDP datagrams (`<= 1400 B`), hardware-accelerated CRC32C, **slice-level XOR FEC** (instant single-packet recovery with zero NACK round-trips), **PLI/IDR** fast recovery for inter-frame codecs, and **AIMD Adaptive Quality / Bitrate** driven by 100 ms receiver telemetry feedback.
- **64-Channel AoIP Bus (Dante / AES67-style):** up to 64 independent per-channel streams (PCM16 / PCM24 / F32, 48 / 96 kHz) on LVSP audio v2, a PTP-style Master Clock (min-RTT gated Kalman filter over PING/PONG), sample-accurate phase-coherent playout across all channels (`T_playout = T_master − latency`), shared ±100 ppm Hermite drift resampler, and a latency-scaled reliability ladder from **2 ms** (0.5 ms chunks, 1+1/2+1 XOR FEC) to **2000 ms** (interleaved FEC + multi-round NACK + duplication) — see [docs/PROTOCOL.md §7](docs/PROTOCOL.md).
- **Virtual 64-Channel OS Audio Device:** LanVid shows up as a 64 In / 64 Out device for any DAW or OBS — Windows ASIO (`LanVidASIO.dll`), macOS CoreAudio HAL plug-in (`LanVidAudio.driver`, Universal), Linux JACK / PipeWire node (`lanvid-audio-bridge`) — bridged to the network through the lock-free shared-memory bus `LanVidAudioBus_v1`; see [docs/VIRTUAL_AUDIO_DEVICE.md](docs/VIRTUAL_AUDIO_DEVICE.md).
- **Synchronized Low-Latency Audio + LAN Auto-Discovery:** 48 kHz Stereo `PCM16` / `Float32` in 2.5 ms or 5 ms UDP packets over the same socket, lock-free SPSC jitter buffer (~12 ms target) with adaptive 1-sample clock-drift compensation, WASAPI Loopback/Mic (Windows), native AAudio (Android), `AVAudioEngine` (Apple), and UDP broadcast LAN peer discovery (port `5099`).
- **Hardware-Accelerated Preview & Desktop/Mobile Apps:**
  - **LanVid Studio (`lanvid_app`):** Unified Sender + Receiver GUI built with vendored Dear ImGui + **Direct3D 11** (`DXGI_SWAP_EFFECT_FLIP_DISCARD`, `ALLOW_TEARING`, automatic GDI fallback) on Windows and **SDL2 + OpenGL 3** on Linux, with live codec selector (`LanVid DCT` / `H.264` / `H.265`), bitrate slider, 64-channel AoIP bus latency (`2–2000 ms`) & LED meter grid, and hardware badge telemetry.
  - **Android App (`android/`):** Kotlin + Jetpack Compose with a broadcast-grade **Blackmagic / NDICam-style HUD** (live tally, audio meters, FPS/bitrate telemetry, shutter/ISO/WB/zoom controls), Camera2 (`YUV_420_888` zero-RGB-copy with full `rowStride`/`pixelStride` + `SENSOR_ORIENTATION` front/back camera rotation & mirror fix), MediaProjection Foreground Service, `ANativeWindow` + native AAudio, live codec switching (`LanVid DCT` / `H.264 HW` / `H.265 HW`), and adaptive AoIP bus latency (`2–2000 ms`).
  - **One-Click Installers & Release Artifacts (`v2.4.0`):**
    - **Windows:** `LanVid_Setup.exe` (Setup Wizard + D3D11/firewall pre-flight + Studio GUI + CLI + `lanvid_core.dll` + 64-ch `LanVidASIO.dll`)
    - **Linux:** `LanVid-Linux-x86_64.run` & `.deb` (`SDL2 + OpenGL3` Studio GUI + CLI + `liblanvid_core.so` + `lanvid-audio-bridge`)
    - **macOS / iOS:** `LanVidSDK-v2.4.0-macOS.pkg` & `.dmg` (Universal `arm64` NEON + `x86_64` AVX2 + `LanVidCore.xcframework` + SwiftPM `LanVidKit` + `LanVidAudio.driver`)
    - **Android:** `LanVid-Android-v2.4.0.apk` (Universal `arm64-v8a` + `armeabi-v7a`, also published as `LanVid-Android.apk`) & `lanvid-sdk-2.4.0.aar`
    - **Cross-Platform SDK:** `LanVidSDK-Universal.zip` (`C ABI`, `C++17`, `Swift`, `Kotlin`, `Python`)

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
├── CMakeLists.txt           # Root CMake build (Core static/shared, GUI, CLI, Tests, SDK install)
├── cmake/                   # Compiler flags & optimization presets
├── docs/
│   └── PROTOCOL.md          # LVSP v1 wire format, headers, XOR FEC, Feedback & Audio specs
├── include/
│   └── lanvid_c_api.h       # Desktop/App C ABI header
├── src/
│   ├── common/              # CPU SIMD dispatch, CRC32C, monotonic clock, Image buffers
│   ├── codec/               # Color (Scalar/AVX2/NEON), 2D LLM DCT/IDCT, Slice/Frame codec, HW H.264/H.265 backends
│   ├── parallel/            # Lock-free work pool for slice-level parallelism
│   ├── audio/               # 64-ch AoIP bus (MasterClock, AudioSyncMatrix, Tx/Rx FEC+NACK) + AudioJitterBuffer
│   ├── net/                 # UDP sockets (Win32/POSIX sendmmsg/recvmmsg), Packetizer/FEC, Assembler, AIMD, Discovery
│   ├── core/                # Shared C ABI implementation (lanvid_core.dll / liblanvid_core.so / .dylib)
│   ├── platform/            # DXGI/GDI screen capture, Media Foundation cam, WASAPI audio, D3D11/SDL2 preview
│   ├── gui/                 # LanVid Studio (Dear ImGui + Win32 D3D11 / Linux SDL2 OpenGL3)
│   └── app/                 # CLI sender / receiver, Y4M/PPM file source, telemetry stats
├── sdk/                     # Cross-Platform LanVidSDK v2.4.0
│   ├── README.md            # Developer Quick Start (C++17, Swift, Kotlin, Python)
│   ├── build_sdk.py         # Unified SDK builder & external find_package() verifier
│   ├── include/lanvid/      # Self-contained C ABI headers (lanvid_sdk.h, lanvid_types.h, lanvid_export.h)
│   ├── cpp/                 # Header-only C++17 RAII wrapper (lanvid.hpp)
│   ├── apple/               # SwiftPM LanVidKit, XCFramework builder, macOS Universal .pkg/.dmg installer & SwiftUI demos
│   ├── android/             # Android Library module (:lanvid-sdk -> .aar with arm64-v8a, armeabi-v7a, x86_64)
│   ├── cmake/               # LanVidSDKConfig.cmake.in & lanvid-sdk.pc.in
│   ├── python/              # Zero-dependency ctypes + optional NumPy bindings (lanvid_sdk.py)
│   └── examples/cpp_client/ # Standalone external CMake consumer demo
├── android/                 # Standalone LanVid Studio Android app (Kotlin + Compose + NDK)
├── installer/               # Windows (Setup Wizard + Inno Setup + preflight.ps1) & Linux (.run / .deb / .AppImage)
├── tests/                   # 43 unit & loopback integration tests (DCT + H.264/H.265 + 64-ch AoIP + Virtual Audio + Android YUV + SDK)
├── bench/                   # 1080p / 4K SIMD vs Scalar codec benchmark
└── third_party/imgui/       # Vendored Dear ImGui v1.91.8 (MIT) for self-contained Desktop GUI builds
```

---

## Quick Build

### Windows (MSVC 2022 + Ninja)
```powershell
# Run from "x64 Native Tools Command Prompt for VS 2022" (or call vcvars64.bat)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run all 43 unit tests & 4K benchmark
.\build\lanvid_tests.exe
.\build\bench_codec.exe

# Launch LanVid Studio GUI or CLI (DCT, H.264, H.265)
.\build\lanvid_app.exe
.\build\sender.exe --list-devices
.\build\sender.exe --host 127.0.0.1 --port 5000 --codec h264 --bitrate 8000 --fps 60
.\build\receiver.exe --port 5000 --preview
```

### Linux (Ubuntu / Debian / Arch / Fedora)
```bash
# Install optional GUI dependencies (SDL2 + OpenGL) for LanVid Studio
sudo apt-get install -y build-essential cmake ninja-build libsdl2-dev libgl1-mesa-dev

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel "$(nproc)"
ctest --test-dir build --output-on-failure

# Or build .deb / .AppImage / self-extracting .run installer
./installer/linux/build_linux_packages.sh
```

### macOS (Apple Silicon M1–M4 & Intel)
```bash
# Build Universal Core SDK + XCFramework (iOS + macOS) + single-file .pkg / .dmg installer
python3 sdk/build_sdk.py
# Or run directly:
./sdk/apple/build_macos_pkg.sh
```

### Android (App `.apk` & Library `.aar`)
```bash
# Universal signed APK (arm64-v8a NEON + armeabi-v7a)
cd android && ./gradlew exportUniversalApk

# Android SDK Library AAR (arm64-v8a + armeabi-v7a + x86_64)
cd sdk/android && ./gradlew exportAar
```

---

## Documentation

- **SDK Developer Guide (C++17, Swift iOS/macOS, Kotlin Android, Python):** [sdk/README.md](sdk/README.md)
- **Wire Protocol & Codec Specification (LVSP v1 / LVC):** [docs/PROTOCOL.md](docs/PROTOCOL.md)
- **Virtual 64-Channel OS Audio Devices (ASIO / CoreAudio / JACK & PipeWire):** [docs/VIRTUAL_AUDIO_DEVICE.md](docs/VIRTUAL_AUDIO_DEVICE.md)

## License

Licensed under the [MIT License](LICENSE). Third-party Dear ImGui (`third_party/imgui/`) is included under its original MIT License.
