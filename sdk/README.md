# LanVid Video SDK (`v3.0.0`) & CrossLink Audio SDK (`v1.0.0`) — Developer Guide

Два повністю відокремлених, взаємно очищених C++20 / C ABI SDK без спільних глобальних станів і без перехресних залежностей:

1. **LanVid Video SDK (`v3.0.0`)** — відео по LAN (`LVC DCT` AVX2/NEON, HW `H.264`/`H.265`, XOR FEC, AIMD/PLI), **2-канальне стерео аудіо** (48 kHz PCM16/F32), Multi-Cam (до 16 камер), `VideoPlayoutBuffer`, ISO Recorder, Virtual Camera `VideoBus`, Tally та Duplex Intercom.
2. **CrossLink Audio SDK (`v1.0.0`)** — 64-канальний фазово-когерентний студійний AoIP (48/96 kHz, `MasterClock`, XOR FEC + NACK, 1+1 Wi-Fi redundancy, Dual-NIC failover), Full-Duplex 64×64 DAW SHM Bus (`CrossLinkAudioBus_v1`), `ChannelPatchbay`, MIDI 2.0, `ControlSync`, Personal Monitor Bus та Layer-2 Grid DSP.

---

## Вибіркове встановлення (Усе разом / Лише відео / Лише аудіо)

### 1. Інтерактивні інсталятори
- **macOS / Linux:** `./scripts/install_sdk.sh` (або `--mode all|video|audio --prefix /usr/local`)
- **Windows PowerShell:** `.\scripts\install_sdk.ps1` (або `-Mode all|video|audio -Prefix C:\SDK`)
- **macOS GUI `.pkg` / `.dmg`:** `./sdk/apple/build_macos_pkg.sh` -> відкрийте `dist/apple/LanVidCrossLinkSDK-v3.0.0-macOS.pkg` та оберіть у меню Customize:
  - ☑ **LanVid Video SDK v3.0.0** (`/usr/local/LanVidSDK`, команда `lanvid-sdk-info`)
  - ☑ **CrossLink Audio SDK v1.0.0** (`/usr/local/CrossLinkSDK`, команда `crosslink-sdk-info`)

### 2. Збірка пакета SDK через Python
```bash
python3 sdk/build_sdk.py --protocol all   --verify-client   # Обидва SDK + тест спільного лінкування (shared + static)
python3 sdk/build_sdk.py --protocol video --verify-client   # Тільки LanVid Video SDK
python3 sdk/build_sdk.py --protocol audio --verify-client   # Тільки CrossLink Audio SDK
```

---

## Quick Start

### 1. C++17 / C++20 (CMake)

**Лише відео + 2ch стерео (LanVid):**
```cmake
find_package(LanVidSDK REQUIRED)
target_link_libraries(my_video_app PRIVATE LanVidSDK::Video)
```
```cpp
#include <lanvid/lanvid.hpp>
lanvid::Runtime rt;
lanvid::Receiver rx;
rx.on_video([](const LV_VideoFrameView& f) { /* zero-copy BGRA/RGBA */ });
rx.on_audio([](const LV_AudioChunkView& a) { /* 1-2 ch stereo PCM16/F32 */ });
rx.start(lanvid::Receiver::default_config(5000));
```

**Лише 64-канальний AoIP + MIDI 2.0 (CrossLink):**
```cmake
find_package(CrossLinkSDK REQUIRED)
target_link_libraries(my_audio_app PRIVATE CrossLinkSDK::Audio)
```
```cpp
#include <crosslink/crosslink.hpp>
crosslink::Runtime rt;
crosslink::Receiver rx;
rx.on_midi([](const CL_MidiEvent& ev) { /* sample-accurate MIDI2 */ });
rx.start(crosslink::Receiver::default_config(5004, 20, 64, 48000));
```

**Обидва протоколи в одному бінарнику (shared або static без колізій символів):**
```cmake
find_package(LanVidSDK REQUIRED)
find_package(CrossLinkSDK REQUIRED)
target_link_libraries(my_studio_app PRIVATE LanVidSDK::Video CrossLinkSDK::Audio)
```

### 2. Swift — iOS 15+ / macOS 12+ (`LanVidKit` та `CrossLinkKit`)
- Пакет `sdk/apple/Package.swift` експортує два незалежних продукти:
  - `LanVidKit` (`LanVidCore.xcframework`) — VideoToolbox / Metal / AVFoundation (без CoreAudio/CoreMIDI).
  - `CrossLinkKit` (`CrossLinkCore.xcframework`) — CoreAudio / CoreMIDI (без VideoToolbox/Metal).
- Збірка XCFramework: `./sdk/apple/build_xcframework.sh --protocol all|video|audio`.

### 3. Android Kotlin AAR (`sdk/android/`)
- `:lanvid-sdk` -> `dist/android/lanvid-video-sdk-3.0.0.aar` (`com.lanvid.sdk.*`, зібрано з `LANVID_SDK_COMPONENTS=VIDEO`)
- `:crosslink-sdk` -> `dist/android/crosslink-audio-sdk-1.0.0.aar` (`com.crosslink.sdk.*`, зібрано з `LANVID_SDK_COMPONENTS=AUDIO`)
```bash
cd sdk/android
gradle --no-daemon :lanvid-sdk:exportAar :crosslink-sdk:exportAar
```

### 4. Python (`ctypes` + опціональний `numpy`)
```python
# Відео + 2ch стерео (LanVid v3.0.0)
from lanvid_sdk import LanVidSDK, LanVidSender, LanVidReceiver

# 64-канальний AoIP + MIDI 2.0 + Audio Node Discovery (CrossLink v1.0.0)
from crosslink_sdk import CrossLinkSDK, CrossLinkSender, CrossLinkReceiver
```
