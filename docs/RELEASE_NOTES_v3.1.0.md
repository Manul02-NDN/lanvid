# LanVid Video v3.1.0 & CrossLink Audio v1.0.0

## Highlights

- **LanVid Video SDK `v3.1.0` (C ABI `v3.x` compatible):**
  - Strict zero-frame VideoToolbox lookahead (`MaxFrameDelayCount = 0`), low-latency rate control, and `CVImageBufferGetCleanRect` hardware decode cropping on Apple platforms.
  - Dynamic Android `AMediaCodec_getInputFormat` resolution via `dlsym` + NV12 stride/slice-height edge padding eliminating chroma shift and green bottom bars on hardware-aligned Android encoders.
  - Runtime C ABI controls `lv_sender_set_codec` and `lv_sender_set_fps` with automatic live encoder reconfiguration.
  - Isolated IDR-only `VideoConfigSync` requests (separate revision deduplication and zeroed config fields) preventing camera settings overwrites.
- **LanVid Camera `v3.1.0` (iOS & Android):**
  - Upgraded Pro Cine HUD, EIS latency hints, pinch/pan viewfinder gesture handling, intercom audio session routing, and broadcast screen refinements.
- **CrossLink Audio SDK `v1.0.0`:**
  - Bundled alongside LanVid Video SDK with selectable installation (`ALL` / `VIDEO` / `AUDIO`) across Windows, Linux, macOS Universal `.pkg`/`.dmg`, Apple XCFrameworks, and Android AARs.

## Release Assets

- **macOS Universal Selectable Installer:** `LanVidCrossLinkSDK-v3.1.0-macOS.pkg` & `.dmg`
- **Apple Frameworks:** `LanVidCore.xcframework.zip` & `CrossLinkCore.xcframework.zip`
- **Desktop SDK Archives (Windows & Linux):** `LanVidCrossLinkSDK-v3.1.0-*.zip`, `LanVidSDK-v3.1.0-*.zip`, `CrossLinkSDK-v1.0.0-*.zip`
- **Android:** `LanVid-Camera-v3.1.0-android.apk`, `lanvid-video-sdk-3.1.0.aar`, `crosslink-audio-sdk-1.0.0.aar`
- **Checksums:** `SHA256SUMS.txt`
