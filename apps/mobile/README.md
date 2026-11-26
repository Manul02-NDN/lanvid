# LanVid Mobile Apps (`apps/mobile/`)

Each mobile application is built on top of the shared **`LanVidCore`** C ABI (`sdk/include/lanvid/lanvid_sdk.h`):

1. **`apps/mobile/lanvid_camera/` (`LanVid Camera` — iOS & Android):**
   - Low-latency mobile cinema camera streaming over Wi-Fi / USB-LAN to **`LanVid Video`** (`5006` video, `5022` Tally & `kVideoConfigSync`, `5023` discovery).
   - Native projects: `mobile/ios_camera/` (Swift/Metal/VideoToolbox) and `android/` (Kotlin/Camera2/MediaCodec).

2. **`apps/mobile/lanvid_monitor/` (`LanVid Monitor` — iOS & Android):**
   - Personal 64->2 IEM monitor mixer with server DSP & deterministic playout connecting to **`LanVid Audio`** (`5020` monitor, `5021` discovery).
   - Native projects: `mobile/ios/` (SwiftUI/AVAudioEngine) and `mobile/android/` (Kotlin/AAudio).

3. **`apps/mobile/lanvid_intercom_mobile/` (`LanVid Intercom` Beltpack — iOS & Android):**
   - Dedicated beltpack client (no video camera capture) connecting to **`LanVid Intercom`** (`5024` intercom, `5025` discovery) or **`LanVid Video`** (`5022`) via `lv_tally_intercom_create_camera()`.
   - Provides large tactile **PTT / LATCH**, **Director / Group A / Group B / All Call** channel selector, headphone volume, and full-width **TALLY / ON AIR** indicator.
