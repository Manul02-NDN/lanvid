# LanVid v2.6.0 — Studio, Monitor & Opus

> 🇺🇦 [Українською](#українською) · 🇬🇧 [English](#english)

---

## Українською

**LanVid 2.6.0** додає персональний моніторинг на смартфонах (**LanVid Monitor** для iOS та Android), кодек Opus для аудіо,
режим MIDI Only з затримкою близько 1,5 мс і виправлення, що усувають «сипучий» і сповільнений звук.

### Нове

- **Кодек Opus (libopus 1.5.2, CELT 2,5 / 5 мс, Fullband 48 кГц) для AUDIO2** замість ADPCM: низька затримка кодування,
  приховування втрат пакетів (PLC), профілі бітрейту на канал: Lossless / 256 / 192 / 128 / 96 / 64 кбіт/с.
- **LanVid Monitor — персональний моніторинг для iOS та Android** (`mobile/`):
  - автопошук LanVid Studio в Wi-Fi (mDNS `_lanvid._udp`) в один тап, або підключення за IP;
  - екран **Channels Mix**: канали з ПК з назвами та живими VU-метрами, вертикальні фейдери, панорама L/R,
    кнопки **M** (Mute) / **S** (Solo), якість стереопотоку 128 / 192 / 256 кбіт/с або Lossless і буфер 5 / 10 / 15 / 20 мс;
  - екран **Monitor FX — Server DSP**: HPF (Off / 40 / 80 / 120 Гц), 3-смуговий еквалайзер, компресор з індикатором
    Gain Reduction, Master Limiter та гучність у навушниках;
  - **серверний 64→2 DSP-мікшер** (`PersonalMonitorBus`, до 8 телефонів): мікс і обробка виконуються на ПК, на телефон
    іде лише готовий стереосигнал; усі зміни надсилаються миттєво пакетом `kMonitorControl`;
  - низькозатримкове відтворення: Android — AAudio `PERFORMANCE_MODE_LOW_LATENCY` з C++ (JNI), iOS — `AVAudioEngine`
    з буфером вводу-виводу 5 мс; фіксований буфер: обраний 5–20 мс і є затримкою моніторингу.
- **Режим MIDI Only (≈1,5 мс)**: ультранизька затримка для MIDI без аудіо та відео.
- **Віртуальний апаратний пристрій LanVid MIDI** (LanVid MIDI In / Out) для Windows MIDI Services та macOS CoreMIDI.
- **48 кГц `MidiDejitterQueue`**: MIDI відтворюється зі статичною затримкою, синхронною з аудіо з точністю до семпла,
  і поглинає джитер Wi-Fi.
- **Двостороння синхронізація налаштувань у реальному часі (`kControlSync`)** між вузлами, із захистом від зациклення.
- **mDNS `_lanvid._udp`**: LanVid Studio анонсує сервер мобільного моніторингу (macOS — Bonjour, Windows 10 1809+).
- **Режим затримки Fixed (Ethernet) / Adaptive (Wi-Fi)** і індикатор реальної затримки буфера в Studio.
- Новий інтерфейс **LanVid Studio** (світлий лавандовий дизайн) і керування бітрейтом на канал.

### Виправлення

- Усунено «сипучий» і сповільнений звук: один споживач на кожен потік, коректне перетворення частоти пристрою
  (44,1 ↔ 48 кГц) і точне тактування мікшера на 48 000 кадрів/с.
- Усунено аварійне завершення Studio при перемиканні «Тільки аудіо» (use-after-free); якщо захоплення екрана недоступне
  (немає дозволу macOS), звук продовжує передаватися, а в інтерфейсі з'являється зрозуміле попередження.
- Стабільніша синхронізація MasterClock на старті.

### Файли релізу

| Платформа | Файл |
|---|---|
| Windows | `LanVid_Setup_v2.6.0.exe`, `LanVidSDK-v2.6.0-Windows.zip` |
| macOS | `LanVid-Studio-v2.6.0-macOS.dmg`, `LanVid-Studio-v2.6.0-macOS.pkg` (Studio + CoreAudio HAL «LanVid Virtual 64ch»; віртуальні порти CoreMIDI створює Studio), `LanVidSDK-v2.6.0-macOS.pkg/.dmg`, `LanVid-Studio-macOS.zip` |
| Android | `LanVid-Monitor-v2.6.0-android.apk`, `LanVid-Android-v2.6.0.apk` (камера/стрімінг), `lanvid-sdk-2.6.0.aar` |
| iOS | `LanVid-Monitor-v2.6.0-iOS.zip` (`.xcarchive`), `LanVid-Monitor-v2.6.0-iOS-unsigned.ipa`, `LanVidCore.xcframework.zip` |
| Linux | `lanvid-v2.6.0-linux-x86_64.tar.gz`, `LanVid-Linux-x86_64.run` |
| Усі | `LanVidSDK-Universal.zip`, `SHA256SUMS.txt` |

> Збірки для iOS та macOS підписані ad-hoc / не підписані: `.xcarchive` та `.ipa` потрібно підписати вашим Apple Developer
> сертифікатом (Xcode → Organizer → Distribute App). APK підписано ключем релізу CI.

---

## English

**LanVid 2.6.0** adds personal monitoring on smartphones (**LanVid Monitor** for iOS and Android), the Opus audio codec,
a MIDI Only mode with ~1.5 ms latency, and fixes for crackling / slowed-down audio.

### What's new

- **Opus codec (libopus 1.5.2, CELT 2.5 / 5 ms, Fullband 48 kHz) for AUDIO2** replacing ADPCM: low encoding delay,
  packet-loss concealment (PLC), per-channel bitrate profiles: Lossless / 256 / 192 / 128 / 96 / 64 kbps.
- **LanVid Monitor — personal monitoring for iOS and Android** (`mobile/`):
  - one-tap discovery of LanVid Studio on Wi-Fi (mDNS `_lanvid._udp`), or connect by IP;
  - **Channels Mix** screen: PC channels with names and live VU meters, vertical faders, L/R pan, **M** (Mute) / **S** (Solo),
    stereo stream quality 128 / 192 / 256 kbps or Lossless, and buffer size 5 / 10 / 15 / 20 ms;
  - **Monitor FX — Server DSP** screen: HPF (Off / 40 / 80 / 120 Hz), 3-band EQ, compressor with a Gain Reduction meter,
    master limiter and headphone volume;
  - **server-side 64→2 DSP mixer** (`PersonalMonitorBus`, up to 8 phones): mixing and processing run on the PC and the
    phone only receives the finished stereo bus; every change is sent instantly as `kMonitorControl`;
  - low-latency playback: Android uses AAudio `PERFORMANCE_MODE_LOW_LATENCY` from C++ (JNI), iOS uses `AVAudioEngine` with
    a 5 ms IO buffer; the buffer is fixed, so the 5–20 ms you pick is the monitoring latency.
- **MIDI Only mode (~1.5 ms)**: ultra-low latency for MIDI without audio and video.
- **Virtual hardware MIDI device LanVid MIDI** (LanVid MIDI In / Out) for Windows MIDI Services and macOS CoreMIDI.
- **48 kHz `MidiDejitterQueue`**: MIDI plays out with a static delay locked to the audio with sample accuracy and absorbs
  Wi-Fi jitter.
- **Real-time two-way settings sync (`kControlSync`)** between nodes, with loop protection.
- **mDNS `_lanvid._udp`**: LanVid Studio announces its mobile monitor server (macOS Bonjour, Windows 10 1809+).
- **Fixed (Ethernet) / Adaptive (Wi-Fi) latency mode** and a live actual-buffer readout in Studio.
- New **LanVid Studio** interface (soft lavender design) and per-channel bitrate control.

### Fixes

- Crackling and slowed-down audio fixed: one consumer per stream, correct device sample-rate conversion (44.1 ↔ 48 kHz)
  and exact mixer clocking at 48,000 frames/s.
- Fixed a Studio crash when toggling Audio Only (use-after-free); if screen capture is unavailable (macOS permission
  missing), audio keeps streaming and the UI shows a clear warning.
- More robust MasterClock synchronisation at start-up.

### Release files

| Platform | Files |
|---|---|
| Windows | `LanVid_Setup_v2.6.0.exe`, `LanVidSDK-v2.6.0-Windows.zip` |
| macOS | `LanVid-Studio-v2.6.0-macOS.dmg`, `LanVid-Studio-v2.6.0-macOS.pkg` (Studio + CoreAudio HAL "LanVid Virtual 64ch"; Studio creates its CoreMIDI virtual ports at run time), `LanVidSDK-v2.6.0-macOS.pkg/.dmg`, `LanVid-Studio-macOS.zip` |
| Android | `LanVid-Monitor-v2.6.0-android.apk`, `LanVid-Android-v2.6.0.apk` (camera / streaming), `lanvid-sdk-2.6.0.aar` |
| iOS | `LanVid-Monitor-v2.6.0-iOS.zip` (`.xcarchive`), `LanVid-Monitor-v2.6.0-iOS-unsigned.ipa`, `LanVidCore.xcframework.zip` |
| Linux | `lanvid-v2.6.0-linux-x86_64.tar.gz`, `LanVid-Linux-x86_64.run` |
| All | `LanVidSDK-Universal.zip`, `SHA256SUMS.txt` |

> iOS and macOS builds are ad-hoc signed / unsigned: re-sign the `.xcarchive` / `.ipa` with your own Apple Developer
> certificate (Xcode → Organizer → Distribute App). The APKs are signed with the CI release key.
