# `LanVid Camera` (`apps/mobile/lanvid_camera/`)

Standalone smartphone camera transmitter app built on **`LanVidCore`** (`sdk/include/lanvid/lanvid_sdk.h`):
- Connects to **`LanVid Video`** (UDP `:5006` video, `:5022` Tally + bidirectional `kVideoConfigSync`, `:5023` LAN/mDNS discovery).
- Supports H.264 / HEVC / LanVid DCT, remote resolution/FPS/bitrate/lens control, glass `CAM 1..16` corner badge assigned by studio, fullscreen Tally border (`STBY / PVW / PGM ON AIR`), and duplex IFB intercom.
- Native sources:
  - iOS: `mobile/ios_camera/LanVidCamera/`
  - Android: `android/app/`
