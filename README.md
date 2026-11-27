# LanVid Video SDK (`v3.0.0`) & CrossLink Audio SDK (`v1.0.0`)

[![CI Build & Test](https://github.com/Manul02-NDN/lanvid/actions/workflows/ci.yml/badge.svg)](https://github.com/Manul02-NDN/lanvid/actions/workflows/ci.yml)
[![Release Artifacts](https://github.com/Manul02-NDN/lanvid/actions/workflows/release.yml/badge.svg)](https://github.com/Manul02-NDN/lanvid/releases)
[![App Store — LanVid Camera](https://img.shields.io/badge/App%20Store-LanVid%20Camera-0D96F6?logo=apple&logoColor=white)](https://apps.apple.com/us/app/lanvid-camera/id6839672027)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**Two cleanly separated, zero-cross-dependency ultra-low-latency LAN streaming protocols + `LanVid Camera` mobile app (iOS & Android):**

| Protocol / SDK | Version | Target Library / CMake Package | Capabilities |
|---|---:|---|---|
| **LanVid** *(Video Protocol)* | `v3.0.0` | `liblanvid_core` / `LanVidSDK::Video` | Ultra-low-latency LAN Video (`LVC DCT` AVX2/NEON, HW `H.264`/`H.265`, XOR FEC, AIMD/PLI), **2-channel stereo audio** (48 kHz PCM16/F32), Multi-Cam (16 slots), `VideoPlayoutBuffer`, ISO Recorder, Virtual Camera `VideoBus`, Tally & Duplex Intercom (`lv_*` C ABI, `lanvid::`, `LanVidKit`, `lanvid_sdk.py`, `com.lanvid.sdk`) |
| **CrossLink** *(Audio Protocol)* | `v1.0.0` | `libcrosslink_core` / `CrossLinkSDK::Audio` | 64-channel phase-coherent studio AoIP (48/96 kHz, `MasterClock`, XOR FEC + NACK, 1+1 Wi-Fi redundancy, Dual-NIC failover), Full-Duplex 64×64 DAW SHM Bus (`CrossLinkAudioBus_v1`), `ChannelPatchbay`, `MultiDeviceAudioMatrix`, MIDI 2.0 over IP, `ControlSync`, Personal Monitor Bus, Layer-2 Grid DSP (`cl_*` C ABI, `crosslink::`, `CrossLinkKit`, `crosslink_sdk.py`, `com.crosslink.sdk`) |

Both libraries use isolated internal namespaces (`lv_sys` vs `cl_sys`), hidden symbol visibility (only `lv_*` or `cl_*` exported), and separate OS frameworks (zero CoreAudio/CoreMIDI in `LanVid`; zero VideoToolbox/D3D11/FFmpeg in `CrossLink`), so you can install and link **either protocol standalone or both together in the same process** with zero symbol collisions.

---

## Selectable SDK Installation (All / Video Only / Audio Only)

### Interactive Installer (macOS / Linux / Windows)
```bash
# macOS / Linux (interactive menu: 1=All, 2=LanVid Video only, 3=CrossLink Audio only)
./scripts/install_sdk.sh
# Or non-interactive CLI:
./scripts/install_sdk.sh --mode all   --prefix /usr/local
./scripts/install_sdk.sh --mode video --prefix /usr/local
./scripts/install_sdk.sh --mode audio --prefix /usr/local
```
```powershell
# Windows PowerShell (interactive menu or CLI)
.\scripts\install_sdk.ps1
.\scripts\install_sdk.ps1 -Mode video -Prefix C:\LanVidSDK
.\scripts\install_sdk.ps1 -Mode audio -Prefix C:\CrossLinkSDK
```

### CMake Component Flag (`LANVID_SDK_COMPONENTS`)
```bash
# Build & test BOTH protocols (default)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLANVID_SDK_COMPONENTS=ALL
cmake --build build --parallel
./build/lanvid_tests
./build/crosslink_tests

# Build ONLY LanVid Video + 2ch Stereo SDK
cmake -S . -B build-video -G Ninja -DCMAKE_BUILD_TYPE=Release -DLANVID_SDK_COMPONENTS=VIDEO
cmake --build build-video --parallel && ./build-video/lanvid_tests

# Build ONLY CrossLink Audio / MIDI / Grid SDK
cmake -S . -B build-audio -G Ninja -DCMAKE_BUILD_TYPE=Release -DLANVID_SDK_COMPONENTS=AUDIO
cmake --build build-audio --parallel && ./build-audio/crosslink_tests
```

### Cross-Platform SDK Packager & External Client Verification
```bash
python3 sdk/build_sdk.py --protocol all   --verify-client
python3 sdk/build_sdk.py --protocol video --verify-client
python3 sdk/build_sdk.py --protocol audio --verify-client
```

### macOS Selectable Universal `.pkg` / `.dmg` Installer
```bash
./sdk/apple/build_macos_pkg.sh
# Produces dist/apple/LanVidCrossLinkSDK-v3.0.0-macOS.pkg & .dmg
# macOS Installer Customize screen lets users check LanVid Video, CrossLink Audio, or both!
```

---

## Repository Structure

```text
lanvid/
├── CMakeLists.txt           # Selectable build: ALL / VIDEO (LanVid v3.0.0) / AUDIO (CrossLink v1.0.0)
├── scripts/                 # Selectable interactive installers (install_sdk.sh, install_sdk.ps1)
├── src/
│   ├── common/              # Protocol-neutral primitives (clock, CRC32C, CPU SIMD, RT threads, UDP sockets, NICs)
│   ├── lanvid/              # Pure Video + 2ch Stereo Audio core (codec, net, audio, video, platform, sources, core)
│   └── crosslink/           # Pure 64ch AoIP + MIDI 2.0 + Monitor + Grid core (net, audio, midi, grid, core)
├── sdk/
│   ├── include/lanvid/      # LanVid Video C ABI headers (v3.0.0)
│   ├── include/crosslink/   # CrossLink Audio C ABI headers (v1.0.0)
│   ├── cpp/                 # Header-only C++17/20 RAII wrappers (lanvid.hpp, crosslink.hpp)
│   ├── python/              # Zero-dependency ctypes/NumPy wrappers (lanvid_sdk.py, crosslink_sdk.py)
│   ├── apple/               # SwiftPM (LanVidKit + CrossLinkKit), XCFramework & macOS .pkg builders
│   ├── android/             # Split Android Gradle AAR modules (:lanvid-sdk + :crosslink-sdk)
│   └── examples/            # cpp_video_client, cpp_audio_client, cpp_combined_client (shared + static)
├── tests/
│   ├── lanvid/              # lanvid_tests (DCT/AVX2/NEON, H.264/H.265, FEC, stereo audio, Multi-Cam, Tally/Intercom)
│   └── crosslink/           # crosslink_tests (64ch AoIP, MasterClock, SHM Bus, MIDI2, Monitor, L2 Grid, C ABI)
├── mobile/ios_camera/       # LanVid Camera for iOS (built with LANVID_SDK_COMPONENTS=VIDEO)
├── android/                 # LanVid Camera for Android (built with LANVID_SDK_COMPONENTS=VIDEO)
└── docs/                    # PROTOCOL_LANVID.md, PROTOCOL_CROSSLINK.md, App Store pages
```

---

## Documentation
- **SDK Integration Guide (C++, Swift, Kotlin, Python, Installers):** [sdk/README.md](sdk/README.md)
- **LanVid Video Wire Protocol (LVSP v1 & LVC):** [docs/PROTOCOL_LANVID.md](docs/PROTOCOL_LANVID.md)
- **CrossLink Audio Wire Protocol (CLAP v1):** [docs/PROTOCOL_CROSSLINK.md](docs/PROTOCOL_CROSSLINK.md)
- **LanVid Camera App Store setup:** [mobile/ios_camera/APPSTORE_SETUP.md](mobile/ios_camera/APPSTORE_SETUP.md)

## License
Licensed under the [MIT License](LICENSE). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
