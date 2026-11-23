# LanVidSDK v2.6.0 — Cross-Platform Ultra-Low Latency LAN Video & Audio SDK

**LanVidSDK** надає єдине C++20/C-ABI ядро (intra-only slice-паралельний AVX2/NEON кодек, UDP + XOR FEC + AIMD, 48 kHz PCM16/F32 аудіо, LAN Auto-Discovery) для інтеграції у власні застосунки під **Windows, Linux, macOS, iOS, Android та Python**.

- **Затримка:** `< 1 ms` кодування/декодування 4K на сучасному CPU (AVX2 / ARM64 NEON), `< 4–6 ms` E2E по LAN.
- **Zero-Copy API:** пряме приймання `CVPixelBuffer` (BGRA / NV12), Android `Image` (`YUV_420_888`), `AVFrame` / `obs_source_frame`, та push-колбеки на прийомі без polling-циклів.
- **Стабільний C ABI:** `lanvid_sdk.h` + `lv_check_abi_compatibility()` — без залежності від версії STL.

---

## Quick Start (10 рядків коду)

### 1. C++17 / C++20 (`find_package(LanVidSDK)`)

```cmake
find_package(LanVidSDK REQUIRED)
target_link_libraries(my_app PRIVATE LanVidSDK::Core)
```

```cpp
#include <lanvid/lanvid.hpp>

lanvid::Runtime rt;
lanvid::Receiver rx;
rx.on_video([](const LV_VideoFrameView& f) { /* zero-copy BGRA/RGBA frame */ });
rx.on_audio([](const LV_AudioChunkView& a) { /* 48 kHz stereo PCM16/F32 */ });
rx.start(lanvid::Receiver::default_config(5000));

auto peers = lanvid::Discovery::scan(400);
lanvid::Sender tx;
tx.start(lanvid::Sender::default_config(peers.empty() ? "127.0.0.1" : peers[0].ip, 5000));
tx.push_frame(bgra_ptr, 1920, 1080);
```

### 2. Swift — iOS 15+ та macOS 12+ (`LanVidKit` SwiftPM + XCFramework + One-Click `.pkg`)

#### Встановлення на MacBook в один клік (`LanVidSDK-v2.6.0-macOS.pkg`)
1. На MacBook (Apple Silicon M1–M4 або Intel) запустіть `python3 sdk/build_sdk.py` або `./sdk/apple/build_macos_pkg.sh` (або подвійний клік по `sdk/apple/installer/Install_LanVidSDK.command` у Finder) — це згенерує **`dist/apple/LanVidSDK-v2.6.0-macOS.pkg`** та **`dist/apple/LanVidSDK-v2.6.0-macOS.dmg`**.
2. Відкрийте `LanVidSDK-v2.6.0-macOS.pkg` подвійним кліком у Finder — майстер `Installer.app` автоматично:
   - перевірить архітектуру (`arm64` NEON / `x86_64` AVX2), версію macOS (`>= 12.0`), порти UDP `5000`/`5099` та Xcode Command Line Tools;
   - встановить універсальну бібліотеку, `LanVidCore.xcframework`, `LanVidKit` Swift Package, CMake/pkg-config симлінки (вкл. `/opt/homebrew/`), `import lanvid_sdk` для Python 3 та винятки в macOS Firewall.
3. Перевірте встановлення, активний SIMD та loopback self-test однією командою у Терміналі:

```bash
lanvid-sdk-info
```

4. Підключіть `/usr/local/LanVidSDK/apple` як локальний Swift Package у Xcode (або `LanVidCore.xcframework`):

```swift
import LanVidKit

let receiver = try LanVidReceiver()
try receiver.start(config: .init(bindPort: 5000, enableAudioPlayback: true))
for await pixelBuffer in receiver.videoFrames() {
    metalRenderer.enqueue(pixelBuffer, in: metalView) // Metal MTKView
}

// Відправка з камери / ReplayKit (zero-copy CVPixelBuffer BGRA або NV12):
let sender = try LanVidSender()
let peers = await LanVidDiscovery.scanOnce()
try sender.start(config: .init(destHost: peers.first?.ip ?? "192.168.1.50"))
try sender.push(sampleBuffer: sampleBuffer)
```

### 3. Kotlin — Android (`lanvid-sdk-2.6.0.aar`)

Додайте `dist/android/lanvid-sdk-2.6.0.aar` у `libs/` та `implementation(files("libs/lanvid-sdk-2.6.0.aar"))`:

```kotlin
import com.lanvid.sdk.*

// Прийом у SurfaceView + нативний низьколатентний AAudio:
val receiver = LanVidReceiver()
lanVidSurfaceView.attach(receiver)
receiver.start(port = 5000, enableAudioPlayback = true)

// Передача камери (YUV_420_888 без конвертації в RGB):
val peers = LanVidDiscovery.scan(400)
val sender = LanVidSender()
sender.start(destIp = peers.firstOrNull()?.ip ?: "192.168.1.50", destPort = 5000)
sender.pushYuvImage(camera2Image) // прямий zero-copy шлях із Camera2/CameraX
```

### 4. Python (`ctypes` + `numpy`)

```python
import numpy as np
from lanvid_sdk import LanVidSender, LanVidReceiver, LanVidSDK

with LanVidReceiver(bind_port=5000) as rx:
    rx.on_video(lambda frame, w, h, fid: print(f"Got frame {fid}: {frame.shape}"))
    peers = LanVidSDK().discover_peers(300)
    with LanVidSender(dest_ip=peers[0]["ip"], dest_port=5000) as tx:
        tx.push_frame(np.zeros((1080, 1920, 4), dtype=np.uint8))
```

### 5. 64-канальна AoIP шина (C ABI, однаково для C++ / Swift / Kotlin / Python)

```c
LV_AudioBusConfig bus;
lv_audio_bus_config_default(&bus);           /* 48 kHz, 20 ms, 64 канали, блоки по 240 */
bus.latency_ms = 25;                          /* 2 (Ultra Low) .. 2000 (Bulletproof) */
lv_sender_configure_audio_bus(tx, &bus);      /* до lv_sdk_sender_start() */
lv_receiver_configure_audio_bus(rx, &bus);

/* Відправка: окремий канал з будь-якого потоку або одразу N каналів (ASIO/CoreAudio/JACK) */
lv_sender_push_audio_channel(tx, 17, mono_f32, 48, LV_AUDIO_FORMAT_F32, LV_MEDIA_TS_AUTO);
lv_sender_push_audio_multichannel(tx, interleaved, 64, 240, LV_AUDIO_FORMAT_PCM24_PACKED,
                                  LV_AUDIO_LAYOUT_INTERLEAVED, LV_MEDIA_TS_AUTO);

/* Прийом: фазово-вирівняний блок у колбеку АБО pull з аудіоколбеку пристрою */
lv_receiver_pop_audio_multichannel(rx, out_f32, 64, frames, LV_AUDIO_LAYOUT_PLANAR);
lv_receiver_set_audio_latency_ms(rx, 500);    /* на льоту, без розриву з'єднання */
LV_AudioChannelStats st; lv_receiver_get_channel_stats(rx, 17, &st); /* dBFS, втрати, FEC, NACK, джитер */
```

`LV_MEDIA_TS_AUTO` прив'язує перший push до Master Clock відправника і далі веде позицію безперервно; для DAW
передавайте власну позицію в семплах (`lv_sender_get_master_clock_samples()`). Колбек і pull-режим
взаємовиключні (`pop` повертає `LV_ERR_UNSUPPORTED`, поки колбек зареєстровано). Нові функції лише додані —
`LV_SDK_ABI_VERSION` = 1 без змін, існуючі структури не змінено.

---

## Структура дистрибутиву `dist/LanVidSDK-v2.6.0/`

- `include/lanvid/` — публічні заголовки `lanvid_export.h`, `lanvid_types.h`, `lanvid_sdk.h`, `lanvid.hpp`
- `bin/` та `lib/` — скомпільована бібліотека `lanvid_core.dll` / `liblanvid_core.so` / `liblanvid_core.dylib`
- `lib/cmake/LanVidSDK/` — `LanVidSDKConfig.cmake`, `LanVidSDKTargets.cmake`
- `lib/pkgconfig/lanvid-sdk.pc` — файл конфігурації для `pkg-config`
- `apple/` — `Package.swift`, `LanVidKit`, `build_xcframework.sh`, SwiftUI-приклади (`iOSCameraSender`, `MacReceiverMetal`)
- `android/` — `lanvid-sdk-2.6.0.aar` (`arm64-v8a`, `armeabi-v7a`, `x86_64`) + Kotlin/JNI вихідний модуль
- `cpp/` та `examples/cpp_client/` — C++17 wrapper і зовнішній приклад для `find_package(LanVidSDK)`
- `python/` — `lanvid_sdk.py`

## Збірка дистрибутиву

```bash
# Збірка для поточної ОС + компіляція і запуск зовнішнього C++ клієнта
python sdk/build_sdk.py --verify-client

# Збірка Android AAR (arm64-v8a + armeabi-v7a + x86_64)
cd sdk/android && gradle exportAar

# Збірка Apple XCFramework + macOS one-click інсталятора (.pkg + .dmg) на MacBook
./sdk/apple/build_macos_pkg.sh
# або автоматично в складі повного пакета SDK:
python3 sdk/build_sdk.py
```
