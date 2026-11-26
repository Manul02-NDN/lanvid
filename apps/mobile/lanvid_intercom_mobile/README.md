# `LanVid Intercom` Mobile Beltpack (`apps/mobile/lanvid_intercom_mobile/`)

Lightweight smartphone Beltpack client (without camera capture) for audio engineers, stage managers, and presenters.
Connects to **`LanVid Intercom`** (default port `5024`, discovery `5025`) or **`LanVid Video`** (`5022`) using `lv_tally_intercom_create_camera()`, `lv_intercom_set_state()`, `lv_intercom_push_mic_pcm()`, and `lv_intercom_pull_rx_pcm()`.

- iOS SwiftUI view: `ios/BeltpackIntercomView.swift`
- Android Compose view: `android/BeltpackIntercomScreen.kt`
