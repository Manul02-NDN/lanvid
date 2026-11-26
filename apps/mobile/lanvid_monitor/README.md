# `LanVid Monitor` (`apps/mobile/lanvid_monitor/`)

Personal 64->2 studio IEM monitor mixer built on **`LanVidCore`** (`sdk/include/lanvid/lanvid_sdk.h`):
- Connects to **`LanVid Audio`** (`:5020` monitor stream, `:5021` discovery, `_lanvid._udp` mDNS `role=monitor`).
- Features Dante-style 1+1 Wi-Fi redundancy, server-side HPF/EQ/Compressor DSP, solo/mute per channel, and AAudio / AVAudioEngine low-latency output.
- Native sources:
  - iOS: `mobile/ios/LanVidMonitor/`
  - Android: `mobile/android/`
