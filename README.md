# LanVid

**Ultra-Low Latency Slice-Based Intra-Frame LAN Video & Audio Streaming Protocol, Cross-Platform Apps (`LanVid Studio`) & Multi-Platform SDK (`C++20 / C ABI / Swift / Kotlin / Python`).**

LanVid is engineered from scratch in dependency-free C++20 for sub-frame **4K60** local network streaming (**< 1 ms** codec latency, **< 4–6 ms** glass-to-glass LAN latency) with zero GOP buffering, instant packet-loss recovery, and synchronized low-latency audio.

---

## Key Features & Measured Benchmarks

- **Intra-Frame Only (`I-Frame Only`):** Zero inter-frame dependencies (`no P/B frames`), zero GOP buffering latency, and zero cascading artifacts after packet loss.
- **Slice-Parallel SIMD Codec (`LVC`):** Every frame is partitioned into independent 16-row horizontal slices encoded/decoded in parallel across all CPU cores with runtime-dispatched **x86_64 AVX2** and **ARM64 NEON** (`16-bit` Loeffler LLM 2D DCT/IDCT + fixed-point `BGRA/RGBA <-> YCbCr 4:2:0/4:2:2/4:4:4` + direct zero-RGB `YUV_420_888 / NV12 / NV21` input).
- **Resilient UDP Transport (`LVSP v1`):** Raw MTU-safe UDP datagrams (`<= 1400 B`), hardware-accelerated CRC32C, **slice-level XOR FEC** (instant single-packet recovery with zero NACK round-trips), and **AIMD Adaptive Quality** driven by 100 ms receiver telemetry feedback.
- **Synchronized Low-Latency Audio + LAN Auto-Discovery:** 48 kHz Stereo `PCM16` / `Float32` in 2.5 ms or 5 ms UDP packets over the same socket, lock-free SPSC jitter buffer (~12 ms target) with adaptive 1-sample clock-drift compensation, WASAPI Loopback/Mic (Windows), native AAudio (Android), `AVAudioEngine` (Apple), and UDP broadcast LAN peer discovery (port `5099`).
- **Hardware-Accelerated Preview & Desktop/Mobile Apps:**
  - **LanVid Studio (`lanvid_app`):** Unified Sender + Receiver GUI built with vendored Dear ImGui + **Direct3D 11** (`DXGI_SWAP_EFFECT_FLIP_DISCARD`, `ALLOW_TEARING`, automatic GDI fallback) on Windows and **SDL2 + OpenGL 3** on Linux.
  - **Android App (`android/`):** Kotlin + Jetpack Compose + Camera2 (`YUV_420_888` zero-RGB-copy) + MediaProjection Foreground Service + `ANativeWindow` + native AAudio.
  - **One-Click Installers (`installer/` & `sdk/apple/installer/`):** Pre-flight environment, port-conflict, firewall, and dependency installers for Windows (`.exe` / Inno Setup), Linux (`.run` / `.deb` / `.AppImage`), and macOS (Universal `.pkg` / `.dmg`).

### Measured Performance (AMD Zen 4, 16 HW Threads, AVX2)

| Workload | Encode (avg / p95) | Decode (avg / p95) | Throughput | E2E UDP Latency |
|---|---:|---:|---:|---:|
| **1080p (`1920×1080`, Q=75, AVX2)** | **0.22 ms** (0.26) | **0.18 ms** (0.20) | **4,471 FPS** | **1.76 ms p50 / 2.08 ms p99** |
| **4K UHD (`3840×2160`, Q=75, AVX2)** | **0.87 ms** (0.99) | **0.80 ms** (0.86) | **1,147 FPS** | **3.45 ms p50 / 3.98–4.84 ms p99** (with D3D11 Preview + XOR FEC) |

*Unit test suite: **31 / 31 passing** (including 4K AVX2 vs Scalar PSNR parity `ΔY = 0.03 dB`, XOR FEC single/double loss, AIMD feedback, lock-free audio drift, zero-copy SDK callbacks, and UDP loopback A/V + LAN discovery).*

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
│   ├── codec/               # Color (Scalar/AVX2/NEON), 2D LLM DCT/IDCT, Quant, Huffman, Slice/Frame codec
│   ├── parallel/            # Lock-free work pool for slice-level parallelism
│   ├── audio/               # Lock-free SPSC AudioJitterBuffer + clock-drift compensation
│   ├── net/                 # UDP sockets (Win32/POSIX sendmmsg/recvmmsg), Packetizer/FEC, Assembler, AIMD, Discovery
│   ├── core/                # Shared C ABI implementation (lanvid_core.dll / liblanvid_core.so / .dylib)
│   ├── platform/            # DXGI/GDI screen capture, Media Foundation cam, WASAPI audio, D3D11/SDL2 preview
│   ├── gui/                 # LanVid Studio (Dear ImGui + Win32 D3D11 / Linux SDL2 OpenGL3)
│   └── app/                 # CLI sender / receiver, Y4M/PPM file source, telemetry stats
├── sdk/                     # Cross-Platform LanVidSDK v1.0.0
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
├── tests/                   # 31 unit & loopback integration tests
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

# Run all 31 unit tests & 4K benchmark
.\build\lanvid_tests.exe
.\build\bench_codec.exe

# Launch LanVid Studio GUI or CLI
.\build\lanvid_app.exe
.\build\sender.exe --list-devices
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

## License

Licensed under the [MIT License](LICENSE). Third-party Dear ImGui (`third_party/imgui/`) is included under its original MIT License.
