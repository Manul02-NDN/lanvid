# LanVid Virtual 64-Channel Audio Device (ASIO / CoreAudio HAL / JACK·PipeWire)

LanVid appears to the operating system as a **64 In / 64 Out** audio device, so any DAW
(REAPER, Ardour, Logic, Cubase) or OBS Studio can send and receive 64 channels over the LanVid
AoIP bus (see [PROTOCOL.md §7](PROTOCOL.md)).

```
 DAW ──► OS virtual device ──► LanVidAudioBus_v1 (shared memory) ──► LanVid Studio / CLI ──► UDP ──►
 DAW ◄── OS virtual device ◄── LanVidAudioBus_v1 (shared memory) ◄── LanVid Studio / CLI ◄── UDP ◄──
        (RT, lock-free)                                               (network, MasterClock, FEC/NACK)
```

Drivers never open sockets. They run inside the DAW / `coreaudiod` and only touch the
shared-memory bus with atomics and `memcpy` — no locks, allocations or system calls on the RT thread.

## Shared-memory bus `LanVidAudioBus_v1` (`src/audio/shm_audio_bridge.*`)

| | |
|---|---|
| Name | `Local\LanVidAudioBus_v1` (Windows `CreateFileMapping`), `/LanVidAudioBus_v1` (`shm_open` + `mmap`) |
| TX ring | DAW outputs → network. SPSC: producer = driver, consumer = host. 64 × 4096 float32 frames |
| RX ring | network → DAW inputs. SPSC: producer = host, consumer = driver. 64 × 4096 float32 frames |
| Cursors | 64-bit monotonic `write_pos` / `read_pos` per ring (+ overrun / underrun counters) |
| Header | `sample_rate`, `buffer_size`, `active_channels`, `driver_running`, driver & host `heartbeat_ms`, `target_latency_ms`, MasterClock `clock_synced` / `clock_skew_ppb`, `driver_name` |
| Size | ≈ 2 MB (layout versioned by `magic` = `LVAB`, `version` = 1) |

A DAW counts as connected while `driver_running = 1` and its heartbeat is younger than 500 ms.
`LANVID_AUDIO_BUS_NAME` overrides the bus name (tests, several instances on one machine).

**Host pump** (`src/audio/shm_audio_host.*`, used by Studio and the CLI):
* TX: forwards everything the DAW writes to `lv_sender_push_audio_multichannel`. The timeline is
  re-anchored to the sender Master Clock when the DAW starts, stops (no data for 250 ms), or its
  device clock stays more than 50 ms away from real time for over 0.5 s. Shorter scheduling
  hiccups are absorbed, because re-anchoring on them would punch holes into the stream.
* RX: keeps the RX ring filled to `max(3 × driver buffer, 512)` frames from
  `lv_receiver_pop_audio_multichannel`, so the network playout is pulled exactly at the DAW
  device rate and the AudioSyncMatrix resampler (±100 ppm) absorbs clock drift.

## Drivers

| OS | Component | Device name | Install |
|---|---|---|---|
| Windows | `LanVidASIO.dll` (64-bit in-process COM, own `iasio_minimal.h`) | `LanVid Virtual ASIO` — `LanVid In 1..64`, `LanVid Out 1..64` | `regsvr32 /s LanVidASIO.dll` (done by `LanVid_Setup.exe`) → `HKLM\SOFTWARE\ASIO\LanVid Virtual ASIO` |
| macOS 12+ | `LanVidAudio.driver` (AudioServerPlugIn, Universal arm64 + x86_64, no kext) | `LanVid Virtual 64ch` — 64-ch Float32 input + output stream | `/Library/Audio/Plug-Ins/HAL/` + `killall -9 coreaudiod` (done by the `.pkg`) |
| Linux | `lanvid-audio-bridge` (libjack via `dlopen`) | JACK / PipeWire node `LanVid-64ch` — `send_1..64`, `recv_1..64` | in `.run` / `.deb`; run `lanvid-audio-bridge` (or `pw-jack lanvid-audio-bridge`), `--pulse` adds a desktop sink |

* **ASIO:** 48 / 96 kHz, buffers 64 / 128 / 256 / 512 / 1024, `ASIOSTFloat32LSB` (default) or
  `ASIOSTInt32LSB` (`HKCU\Software\LanVid\ASIO  SampleFormat = "int32"`). The device clock is a
  high-resolution waitable timer scheduled on QPC (`timeBeginPeriod(1)`, MMCSS "Pro Audio"),
  i.e. the same clock the LanVid sender uses as its Master Clock.
  *Distributing an ASIO driver may require Steinberg's ASIO licensing agreement; "ASIO" is a
  Steinberg trademark.*
* **CoreAudio HAL:** 48 / 96 kHz (switchable from Audio MIDI Setup), zero timestamps on the host
  clock (`mach_absolute_time`). Requires an ad-hoc or Developer ID signature.
* **JACK / PipeWire:** rate and buffer follow the JACK graph; the binary starts without JACK and
  explains what to install (`pipewire-jack` or `libjack-jackd2-0`).

## Usage

**LanVid Studio:** Sender tab → *Джерело шини* = `Virtual Audio Device (ASIO / CoreAudio / PipeWire)`;
Receiver tab → *Вихід шини* = `Virtual Audio Device (DAW inputs)`. The panel shows whether a DAW
is attached, its sample rate, buffer size and ring fill.

**Headless CLI:**

```bash
./build/sender   --host 192.168.1.50 --audio-device virtual --audio-channels 64 --audio-latency 20
./build/receiver --audio-device virtual --audio-latency 20
```

## Verification

* `shm_audio_bus_64ch_lockfree_roundtrip`: two processes' worth of mappings, multi-threaded
  64-ch round trip of 256 000 frames bit-exact; DAW connect / disconnect detection; end-to-end
  DAW → SHM TX → `lv_sender` → UDP → `lv_receiver` → SHM RX → DAW with 64 phase-aligned channels.
* `hal_driver_bundle_loads_and_streams` (macOS): loads the built `LanVidAudio.driver` through its
  factory like `coreaudiod`, walks the property graph, switches 48 ↔ 96 kHz and streams 64
  channels both ways through `ReadInput` / `WriteMix`.
* CI checks the ASIO DLL exports (`dumpbin`), the HAL bundle (`Info.plist`, both Mach-O slices,
  `_LanVidAudioDriverFactory`) and the Linux bridge start-up behaviour.

Not covered automatically: loading by a real DAW, `coreaudiod`'s sandbox, and ASIO hosts on
Windows — these need a manual check on target machines.
