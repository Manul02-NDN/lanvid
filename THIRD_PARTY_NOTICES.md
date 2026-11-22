# Third-Party Notices & License Audit

**LanVid** is licensed under the **MIT License** (see [LICENSE](LICENSE)). The core C++20 video/audio codec, UDP transport, XOR FEC, AIMD controller, lock-free audio jitter buffer, LAN discovery, C ABI, C++17 wrapper, Swift wrapper, and Python bindings are written from scratch with zero third-party runtime dependencies.

Below is the complete audit of bundled third-party code and optional/platform SDKs used across the desktop, mobile, and SDK targets:

---

## 1. Bundled Third-Party Source Code

### Dear ImGui (v1.91.8)
- **Location:** `third_party/imgui/`
- **Used by:** `lanvid_app` (`LanVid Studio` Desktop GUI on Windows and Linux; excluded when building `LANVID_SDK_CORE_ONLY=ON`)
- **License:** MIT License
- **Copyright:** Copyright (c) 2014–2025 Omar Cornut
- **License Text:** See `third_party/imgui/LICENSE.txt`

---

## 2. Internal Test Harness

### `tests/mini_test.hpp`
- **Used by:** `lanvid_tests` (31 unit and loopback integration tests)
- **License:** MIT License (in-repo lightweight C++20 test runner; no external GoogleTest/Catch2 dependency required)

---

## 3. Dynamically Linked / System Platform SDKs & Optional Libraries

The following libraries are **not vendored** in this repository and are provided by the host OS or standard build toolchains:

| Component | Target Platform | Purpose | License |
|---|---|---|---|
| **Windows SDK** (`d3d11`, `dxgi`, `d3dcompiler`, `ws2_32`, `iphlpapi`, `mmdevapi`, `avrt`, `mf*`, `gdi32`) | Windows | Hardware D3D11 preview, DXGI capture, Media Foundation webcam, WASAPI audio, Winsock2 | Microsoft Windows SDK License (system DLLs) |
| **SDL2** (`libsdl2`) | Linux (optional GUI/preview) | Windowing & OpenGL 3 context for `LanVid Studio` | zlib License |
| **Mesa / OpenGL / Khronos GL Headers** | Linux | GPU texture rendering in `LanVid Studio` | MIT / Apache-2.0 / Khronos |
| **X11 / PipeWire / PulseAudio** | Linux | Display & audio integration | MIT / LGPL-2.1+ (dynamically linked system libraries) |
| **Apple Xcode SDK** (`CoreVideo`, `CoreMedia`, `AVFoundation`, `Metal`, `MetalKit`) | macOS / iOS | `LanVidKit` Swift wrapper & Metal/AVAudioEngine integration | Apple SDK License (system frameworks) |
| **Android NDK** (`android`, `aaudio`, `log`, `libc++_shared`) | Android | `ANativeWindow` rendering, low-latency AAudio playback, ARM64 NEON | Apache License 2.0 / NCSA |
| **AndroidX & Jetpack Compose** (`core-ktx`, `activity-compose`, `material3`) | Android (`android/`, `sdk/android/`) | Kotlin UI & Android lifecycle integration | Apache License 2.0 |
| **Python Standard Library (`ctypes`) & Optional `NumPy`** | Cross-platform (`sdk/python/`) | Python SDK bindings (`NumPy` is optional, BSD-3-Clause) | PSF License / BSD-3-Clause |
