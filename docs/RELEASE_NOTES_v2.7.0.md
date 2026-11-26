# LanVid v2.7.0 — Split Studio (`LanVid Audio` & `LanVid Video`), Multi-Cam ISO/vCam & Full AoIP Engine

> 🇺🇦 [Українською](#українською) · 🇬🇧 [English](#english)

---

## Українською

**LanVid 2.7.0** розділяє десктопну студію на два спеціалізовані застосунки — **`LanVid Audio`** (AoIP 64×64, Patchbay/Матриця, Personal Monitor `:5020`, MIDI, Grid L2 256ch) та **`LanVid Video`** (Multi-Cam, ISO-запис MP4/MKV/MOV, віртуальні камери, Intercom/IFB, OBS Studio), із незалежними конфігураціями та портами без жодних конфліктів.

### Нове

- **`LanVid Audio` — повноцінний аудіорушій та 4-сторінковий Soft Pastel Lavender / Dark Studio UI**:
  - **Мережа та Зв'язок**: інтерактивна топологія вузлів (`mDNS :5021` / `Grid L2`), швидкі пресети (*Слухати на ПК*, *Студійний міст 1:1*, *Тільки MIDI*, *+ Підключити за IP*), інспектор потоку з 3 кільцевими індикаторами (Якість %, Затримка ms, Трафік Mbps), вибір Primary/Secondary NIC (1+1 Failover) та двостороння синхронізація `kControlSync`.
  - **Маршрутизація**: візуальний **Patchbay** з Drag-and-Drop кривими Безьє та живими VU-метрами, повна матриця кроспоінтів **64×64** (`MultiDeviceAudioMatrix`), а також **MIDI-роутинг** (`LanVid MIDI (DAW)`, `MidiDejitterQueue`, `MIDI Only ~1.5 ms`, `PANIC`).
  - **Мобільний монітор (`PersonalMonitorServer :5020`)**: картки підключених смартфонів із QR-кодом, редагування назв каналів `1..64`, **Server-Side DSP** (HPF, 3-смуговий EQ, Compressor з індикатором Gain Reduction, Brickwall Limiter) та `1+1 Wi-Fi Redundancy`.
  - **Пристрої та `LanVid Grid (Layer 2 · 256ch)`**: кешований список аудіо/MIDI-пристроїв, віртуальна шина `LanVid Virtual ASIO / 64ch`, підтримка `GridNode` + `GridFailoverController` (Primary/Backup DSP Server).
- **`LanVid Video` — Multi-Cam, ISO-запис та віртуальні камери**:
  - окремі порти (`:5006` відео, `:5022` Tally/Intercom, `:5023` Discovery `role="video"`);
  - одночасний прийом до 16 камер, мультив'ю `1×1 / 2×2 / 3×3`, апаратне декодування, **ISO-запис** у MP4/MKV/MOV та до 8 віртуальних камер ОС.

---

## English

**LanVid 2.7.0** splits the desktop studio into two dedicated applications — **`LanVid Audio`** (64×64 AoIP, Patchbay/Matrix, Personal Monitor `:5020`, MIDI, Grid L2 256ch) and **`LanVid Video`** (Multi-Cam, ISO recording MP4/MKV/MOV, virtual cameras, Intercom/IFB, OBS integration) — running side-by-side with isolated configs and zero port conflicts.

### What's new

- **`LanVid Audio` — full audio engine and 4-page Soft Pastel Lavender / Dark Studio UI**:
  - **Network & Link**: interactive topology canvas (`mDNS :5021` / `Grid L2`), quick presets, stream inspector with 3 telemetry ring gauges (Quality %, Latency ms, Traffic Mbps), Dual-NIC 1+1 failover selector, and bidirectional `kControlSync`.
  - **Routing & Patchbay**: visual Bezier Patchbay with live VU meters, full **64×64** crosspoint matrix (`MultiDeviceAudioMatrix`), and **MIDI Routing** (`LanVid MIDI (DAW)`, `MidiDejitterQueue`, `MIDI Only ~1.5 ms`, `PANIC`).
  - **Personal Monitor (`:5020`)**: connected smartphone cards with QR code, custom channel names `1..64`, **Server-Side DSP** (HPF, 3-band EQ, Compressor with Gain Reduction meter, Brickwall Limiter), and `1+1 Wi-Fi Redundancy`.
  - **Devices & `LanVid Grid (Layer 2 · 256ch)`**: cached audio/MIDI device enumeration, `LanVid Virtual ASIO / 64ch`, and `GridNode` + `GridFailoverController` (Primary/Backup DSP Server).
- **`LanVid Video` — Multi-Cam, ISO recording & virtual cameras**:
  - dedicated ports (`:5006` video, `:5022` Tally/Intercom, `:5023` Discovery `role="video"`);
  - up to 16 concurrent camera slots, `1×1 / 2×2 / 3×3` multiview, hardware decode, **ISO recording** (MP4/MKV/MOV), and up to 8 OS virtual cameras.
