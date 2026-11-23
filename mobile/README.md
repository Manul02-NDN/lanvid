# LanVid Monitor — персональний моніторинг на смартфоні (Android + iOS)

Телефон підключається до **LanVid Studio** на ПК по локальному Wi-Fi і отримує власний моніторинговий мікс.
Мікшування та вся обробка виконуються **на ПК** (`PersonalMonitorBus`, до 8 телефонів): телефон надсилає лише
команди керування (`PacketType::kMonitorControl`), а назад отримує готовий 2-канальний стереопотік `AUDIO2`.

| | Android (`mobile/android`) | iOS (`mobile/ios`) |
|---|---|---|
| UI | Kotlin + Jetpack Compose | Swift + SwiftUI |
| Звук | AAudio `PERFORMANCE_MODE_LOW_LATENCY` з C++ (JNI), без Java у звуковому шляху; буфер = 2 burst | `AVAudioEngine` + `AVAudioSourceNode`, `preferredIOBufferDuration = 0.005` |
| Ядро | `lanvid_core` (статично, CMake) + `monitor_jni.cpp` | `LanVidCore.xcframework` (`scripts/build_core.sh`) |
| Автопошук | `NsdManager` (mDNS `_lanvid._udp`) | `NWBrowser` (Bonjour `_lanvid._udp`) |
| Фон | foreground service `mediaPlayback` | `UIBackgroundModes: audio` |

## Екрани

1. **Підключення** — студії LanVid Studio в мережі (mDNS) в один тап, або підключення за IP:порт (типово `5020`);
   назва телефону для списку на ПК.
2. **Channels Mix** — канали з ПК з назвами та живими VU-метрами; вертикальні фейдери (−60…+12 дБ, подвійний тап = 0 дБ),
   панорама L/R, **M**ute / **S**olo; якість стереопотоку **128 / 192 / 256 кбіт/с на канал** або **Lossless**
   (24-bit PCM) і буфер **5 / 10 / 15 / 20 мс** — змінюються на льоту без перепідключення.
3. **Monitor FX — Server DSP** — HPF (Off / 40 / 80 / 120 Гц), 3-смуговий EQ (Low 100 Гц, Mid з вибором частоти
   300…5000 Гц, High 8 кГц), компресор (Threshold, Ratio, Makeup + індикатор Gain Reduction з ПК),
   Master Limiter (−0.5 dBFS, завжди увімкнений) & Volume з вихідним рівнем.

Кожна зміна миттєво відправляється на ПК; мікс і налаштування обробки зберігаються на телефоні та відновлюються при
наступному підключенні.

## Збірка

**Android** (Android Studio / SDK 37, NDK 28.2, CMake 3.31, JDK 17+):

```bash
cd mobile/android && gradle assembleDebug
```

**iOS** (Xcode 16+, XcodeGen):

```bash
./mobile/ios/scripts/build_core.sh
```

```bash
cd mobile/ios && xcodegen generate && open LanVidMonitor.xcodeproj
```

## На ПК

У LanVid Studio увімкніть **«Мобільний моніторинг»** (UDP `5020`). Studio анонсує себе через mDNS
(`_lanvid._udp`, macOS — Bonjour, Windows 10 1809+ — `DnsServiceRegister`); на Linux підключайтеся за IP.
Звук на телефони надходить, коли працює Audio Matrix.

## Протокол (C ABI, `sdk/include/lanvid/lanvid_sdk.h`)

`lv_monitor_client_connect / set_channel_mix / set_dsp / set_stream / keepalive / request_meta / get_meta /
poll / pop_stereo`. Буфер телефона фіксований (Fixed jitter buffer) — обрані 5…20 мс і є затримкою моніторингу.
