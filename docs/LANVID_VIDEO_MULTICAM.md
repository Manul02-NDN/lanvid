# LanVid Video — Multi-Cam, ISO Recording, OS Virtual Cameras (2026-11-26)

Status report and hand-off notes for the Multi-Cam / ISO / virtual camera / UI rework of **LanVid Video**.

## 1. What is implemented

| Area | Files | State |
|---|---|---|
| Multi-Cam reception (up to 16 slots; one port 5006 demuxed by sender **or** 5006/5008/…; auto-bind, pin by IP, per-slot telemetry, Tally, REC) | `src/video/multicam_manager.*`, `src/net/receiver.*` (shared-socket mode), C ABI `lv_multicam_*` | done, tested |
| ISO recorder: one file per camera, `{Dir}/{Session}_{YYYY-MM-DD_HH-MM-SS}/CAM1_{Label}.mp4/.mkv/.mov`, zero-transcode H.264/HEVC remux, PCM audio, shared start timecode (tmcd / TIMECODE), optional PGM Master Mix, live stats | `src/video/iso_recorder.*` | done, tested (ffprobe/ffmpeg decode, timecode read back) |
| `LanVidVideoBus_v1` shared memory (Cam 1..8 + Program, BGRA ≤1080p / NV12 ≤4K, seqlock, placeholder card) | `src/video/video_bus.*` | done, tested |
| Remote phone control (lens / focus / 360p–4K preset) over the Tally channel, `PacketType::kCameraControl = 0xE`, 3× redundant + de-dup | `src/net/packet.*`, `src/audio/intercom_engine.*`, C ABI `lv_tally_intercom_set_camera_control`, `lv_camera_set_control_callback` | done, tested (core); **phone apps not wired yet** |
| LanVid Video UI: 4 sidebar pages × pill sub-tabs | `src/gui/video_studio_ui.*`, `src/gui/qr_code.*` | done (Mac + Windows build) |
| Windows virtual cameras: DirectShow `LanVidVCam.dll` (`LanVid Cam 1..8` + `LanVid Program Out`) | `drivers/windows_vcam/*` | done, **verified end-to-end on the Windows PC** |
| macOS virtual cameras: CoreMediaIO **Camera Extension** + `LanVid Virtual Camera.app` activator | `drivers/macos_vcam/*`, `drivers/common_vcam/*` | code done and compiled; **activation needs a signed build (see §4)** |
| Linux virtual cameras: `lanvid-vcam-bridge` → v4l2loopback `/dev/video10..18` labelled `LanVid Cam N` | `drivers/linux_vcam/*`, `drivers/common_vcam/*` | code done (syntax-checked on macOS against V4L2 layout); **not run on Linux yet** |
| Installers | `installer/windows/{setup_wizard.cpp,lanvid.iss,build_windows_setup.ps1,preflight.ps1}`, `sdk/apple/build_macos_pkg.sh`, `installer/linux/{build_linux_packages.sh,install_lanvid.sh}` | updated |

### UI structure (LanVid Video)

1. **Камери та Підключення** — *Усі камери (Multiview Grid 1×1/2×2/3×3)*: live preview cards with editable name, resolution/FPS/codec/bitrate/latency, battery/temperature, Virtual-Cam status, per-camera REC, PVW/PGM, remote lens/quality/AF/focus · *Додати та знайти в мережі*: phones found via Tally, "Підключити в Слот N", manual IP pinning, slot release, NIC choice, other video studios, **QR code** (`lanvid://connect?host=…&port=…&tally=…&name=…`).
2. **Запис та Ефір** — *ISO Recorder*: big REC ALL / STOP ALL, session timecode, duration, free disk (red < 20 GB), folder / session name / MP4·MKV·MOV / +PGM, per-camera table (include checkbox, signal, file, size·duration, MB/s, REC) · *Program / Preview*: PVW/PGM monitors, CUT, AUTO (0.5 s), camera bus (click = PVW, double-click = PGM), Tally synced to phones.
3. **Віртуальні камери та Вивід** — *Virtual Cameras*: driver status + "Зареєструвати віртуальні камери", number of devices 0..8 (+ Program Out), placeholder toggle, device → source table · *OBS & Audio*: obs-lanvid plugin status, PGM → OBS, lip-sync ms, camera audio routing + per-camera meters.
4. **Інтерком та Налаштування** — *Duplex Intercom*: director mic / headphones (Windows), ALL CALL (hold), LATCH, IFB volume, Auto-Ducking, per-camera TALK (PTT) / LATCH, operator-speaking indicator · *System*: studio name, ports (Video base / Tally 5022 / Discovery 5023), port mode, slot count, NICs, DSCP AF41 / EF, Wi-Fi 1+1, HW decode, theme.

All settings persist in `lanvid_video_config.json`.

## 2. Test results (2026-11-26)

* **macOS** `./build/lanvid_tests`: 94/95. The one failure, `network_dual_nic_1plus1_failover_zero_dropouts` (personal monitor audio, timing-sensitive), fails the same way on a clean `origin/main` build on this Mac under the current load (3/6 runs vs 2/6 for this branch) — pre-existing flake, not a regression. New tests: `multicam_three_cameras_iso_recording_and_video_bus`, `iso_recorder_mp4_mkv_mov_containers_close_cleanly`, `video_bus_slots_isolated_and_placeholder`, `video_config_slots_iso_vcam_persist`, `camera_remote_control_lens_focus_quality_roundtrip` — all pass.
* **Windows** (studio-pc, MSVC Release) `lanvid_tests.exe`: **92/92**.
* **Windows virtual cameras** `lanvid_vcam_probe.exe` (real DirectShow client: device enumeration → ICaptureGraphBuilder2 → Sample Grabber → Null Renderer, test frames published into the bus): **all 13 checks pass** — 9 devices registered; RGB32 1080p from a 720p camera (scaling, bottom-up orientation, exact colours); NV12 1080×1920 (9:16, letterboxed); YUY2 placeholder on an empty slot; NV12 zero-conversion on Program Out; **NV12 4K60 = 120 frames / 2 s**; default format without `SetFormat`.
* **macOS Camera Extension**: `xcodebuild` (unsigned) builds the app + `com.manul02.lanvid.vcam.extension.systemextension`; the C frame bridge was checked with a stand-alone program (live frame aspect-fit into 1080×1920 with padded stride, placeholder on empty slot).

## 3. Installed / built locally

* `/Applications/LanVid Video.app`, `/Applications/LanVid Audio.app` — Release, ad-hoc signed.
* `/Applications/LanVid Studio.app` is owned by root (installed by the .pkg), so it was **not** replaced. To update it:
  `sudo rm -rf "/Applications/LanVid Studio.app" && sudo cp -R build/"LanVid Studio.app" /Applications/ && sudo codesign --force --deep --sign - "/Applications/LanVid Studio.app"`
* Windows: everything built in `C:\Users\USER\lanvid-audit\build` (incl. `LanVidVCam.dll`, `lanvid_vcam_probe.exe`, `lanvid_video_studio.exe`). `LanVidVCam.dll` is currently registered (regsvr32) **from that build folder** for testing; the installer re-registers its own copy from `{app}`. To remove the test registration: `regsvr32 /u C:\Users\USER\lanvid-audit\build\LanVidVCam.dll`.

## 4. Known limitations / next steps

1. **macOS activation** — macOS 14+ no longer loads CoreMediaIO **DAL** plug-ins (`/Library/CoreMediaIO/Plug-Ins/DAL`), so the virtual cameras are a **Camera Extension** (system extension). It must be signed by a team: open Xcode → Settings → Accounts, add the Apple ID of team `9SN7V4SC2S`, then run
   `drivers/macos_vcam/build_vcam.sh --install` and allow "LanVid Virtual Camera" in System Settings → General → Login Items & Extensions → Camera Extensions. The shared memory name is `<LANVID_MACOS_APP_GROUP>/vbus1` (CMake cache var, default `9SN7V4SC2S.lanvid`) and must match the extension's App Group. For distribution outside this team a Developer ID + notarization is required.
2. **Linux** — run on a real Linux box: `apt install v4l2loopback-dkms`, install the .deb, `systemctl --user enable --now lanvid-vcam-bridge`, check with `v4l2-ctl --list-devices` / `ffplay /dev/video10`.
3. **Phone apps** — Android (`android/app/src/main/cpp/lanvid_jni.cpp`) and iOS LanVid Camera must subscribe to `lv_camera_set_control_callback` and apply lens / focus / quality; they must also open `lanvid://connect?...` from the QR code.
4. Director mic / headphones in the Intercom page are Windows-only (WASAPI); macOS/Linux use LanVid Audio for now.
5. Open audit item (unchanged): H.264 from the Mac (VideoToolbox) is not decoded by the Windows receiver while Windows→Windows works.
6. Windows: 32-bit `LanVidVCam.dll` (for legacy 32-bit apps) is not built; Windows 11 Media Foundation virtual camera (`MFCreateVirtualCamera`) would add native MF apps (Camera app, new Teams in MF-only mode).
