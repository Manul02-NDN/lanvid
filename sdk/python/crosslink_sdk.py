"""
CrossLink Audio SDK v1.0.0 — Python ctypes + optional NumPy Bindings (Windows / Linux / macOS)

Pure ultra-low-latency pro-audio protocol:
  - 64-channel phase-coherent AoIP (48/96 kHz, F32 / PCM24 / PCM16 / Opus / ADPCM)
  - MasterClock sync, XOR FEC + NACK + 1+1 Wi-Fi redundancy & Dual-NIC failover
  - MIDI 2.0 over IP with sample-accurate dejitter queue
  - Personal Monitor Bus & Audio Node LAN Discovery (:5021)

For video + 2-channel stereo streaming, see `lanvid_sdk.py`.
"""

from __future__ import annotations

import ctypes
import os
import platform
from pathlib import Path
from typing import Callable, List, Optional

try:
    import numpy as np  # type: ignore
    _HAS_NUMPY = True
except ImportError:
    np = None  # type: ignore
    _HAS_NUMPY = False

# ---- Constants matching sdk/include/crosslink/crosslink_export.h & crosslink_types.h ----
__version__ = "1.0.0"
CL_SDK_VERSION_MAJOR = 1
CL_SDK_VERSION_MINOR = 0
CL_SDK_VERSION_PATCH = 0

CL_AUDIO_MAX_CHANNELS = 64
CL_AUDIO_FORMAT_PCM16 = 0
CL_AUDIO_FORMAT_F32 = 1
CL_AUDIO_FORMAT_PCM24_PACKED = 2

CL_AUDIO_LAYOUT_INTERLEAVED = 0
CL_AUDIO_LAYOUT_PLANAR = 1
CL_MEDIA_TS_AUTO = 0xFFFFFFFFFFFFFFFF

CL_OK = 0


class CL_AudioBusConfig(ctypes.Structure):
    _fields_ = [
        ("sample_rate", ctypes.c_int32),
        ("latency_ms", ctypes.c_uint32),
        ("num_channels", ctypes.c_int32),
        ("block_frames", ctypes.c_uint32),
    ]


class CL_SenderConfig(ctypes.Structure):
    _fields_ = [
        ("dest_ip", ctypes.c_char * 64),
        ("dest_port", ctypes.c_uint16),
        ("bind_port", ctypes.c_uint16),
        ("bind_ip", ctypes.c_char * 64),
        ("secondary_bind_ip", ctypes.c_char * 64),
        ("mtu", ctypes.c_int32),
        ("sample_rate", ctypes.c_int32),
        ("latency_ms", ctypes.c_uint32),
        ("audio_codec", ctypes.c_int32),
        ("audio_bitrate_kbps", ctypes.c_int32),
    ]


class CL_ReceiverConfig(ctypes.Structure):
    _fields_ = [
        ("bind_ip", ctypes.c_char * 64),
        ("bind_port", ctypes.c_uint16),
        ("discovery_port", ctypes.c_uint16),
        ("sample_rate", ctypes.c_int32),
        ("latency_ms", ctypes.c_uint32),
        ("num_channels", ctypes.c_int32),
        ("block_frames", ctypes.c_uint32),
        ("adaptive_jitter", ctypes.c_int32),
        ("enable_discovery", ctypes.c_int32),
        ("device_name", ctypes.c_char * 128),
    ]


class CL_MultichannelAudioBlock(ctypes.Structure):
    _fields_ = [
        ("data", ctypes.POINTER(ctypes.c_float)),
        ("num_channels", ctypes.c_int32),
        ("num_frames", ctypes.c_int32),
        ("sample_rate", ctypes.c_int32),
        ("reserved", ctypes.c_uint32),
        ("playout_position_samples", ctypes.c_uint64),
        ("block_index", ctypes.c_uint64),
    ]


class CL_AudioChannelStats(ctypes.Structure):
    _fields_ = [
        ("packets_rx", ctypes.c_uint64),
        ("packets_lost", ctypes.c_uint64),
        ("fec_recovered", ctypes.c_uint64),
        ("nack_recovered", ctypes.c_uint64),
        ("nacks_sent", ctypes.c_uint64),
        ("underruns", ctypes.c_uint64),
        ("late_packets", ctypes.c_uint64),
        ("level_dbfs", ctypes.c_float),
        ("active", ctypes.c_int32),
        ("jitter_us", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
    ]


class CL_StreamStats(ctypes.Structure):
    _fields_ = [
        ("state", ctypes.c_int32),
        ("clock_synced", ctypes.c_int32),
        ("rtt_us", ctypes.c_uint32),
        ("clock_skew_ppm", ctypes.c_double),
        ("audio_kbps", ctypes.c_double),
        ("effective_latency_ms", ctypes.c_double),
        ("effective_target_ms", ctypes.c_uint32),
        ("adaptive_jitter", ctypes.c_int32),
        ("audio_packets", ctypes.c_uint64),
        ("audio_bytes", ctypes.c_uint64),
        ("fec_packets", ctypes.c_uint64),
        ("retransmits", ctypes.c_uint64),
        ("midi_events", ctypes.c_uint64),
        ("stream_epoch_resets", ctypes.c_uint64),
    ]


class CL_MidiEvent(ctypes.Structure):
    _fields_ = [
        ("media_ts_us", ctypes.c_uint64),
        ("event_seq", ctypes.c_uint32),
        ("block_offset_frames", ctypes.c_int32),
        ("status", ctypes.c_uint8),
        ("data1", ctypes.c_uint8),
        ("data2", ctypes.c_uint8),
        ("port", ctypes.c_uint8),
    ]


class CL_AudioNode(ctypes.Structure):
    _fields_ = [
        ("ip", ctypes.c_char * 64),
        ("name", ctypes.c_char * 128),
        ("hostname", ctypes.c_char * 128),
        ("audio_driver", ctypes.c_char * 32),
        ("rx_port", ctypes.c_uint16),
        ("tx_port", ctypes.c_uint16),
        ("channels", ctypes.c_uint16),
        ("duplex", ctypes.c_uint8),
        ("driver_connected", ctypes.c_uint8),
        ("rtt_ms", ctypes.c_uint32),
        ("os", ctypes.c_char * 32),
    ]


MC_AUDIO_CB_TYPE = ctypes.CFUNCTYPE(None, ctypes.POINTER(CL_MultichannelAudioBlock), ctypes.c_void_p)
MIDI_CB_TYPE = ctypes.CFUNCTYPE(None, ctypes.POINTER(CL_MidiEvent), ctypes.c_void_p)
STATE_CB_TYPE = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p)


def _as_audio_buffer(samples, fmt: int):
    if _HAS_NUMPY and isinstance(samples, np.ndarray):
        dtype = {
            CL_AUDIO_FORMAT_PCM16: np.int16,
            CL_AUDIO_FORMAT_F32: np.float32,
            CL_AUDIO_FORMAT_PCM24_PACKED: np.uint8,
        }[fmt]
        arr = np.ascontiguousarray(samples, dtype=dtype)
        n = arr.size if fmt != CL_AUDIO_FORMAT_PCM24_PACKED else arr.size // 3
        return arr.ctypes.data_as(ctypes.c_void_p), arr, int(n)
    raw = bytes(samples)
    bps = {CL_AUDIO_FORMAT_PCM16: 2, CL_AUDIO_FORMAT_F32: 4, CL_AUDIO_FORMAT_PCM24_PACKED: 3}[fmt]
    buf = (ctypes.c_uint8 * len(raw)).from_buffer_copy(raw)
    return ctypes.cast(buf, ctypes.c_void_p), buf, len(raw) // bps


def _find_library(explicit_path: Optional[str] = None) -> str:
    if explicit_path:
        return explicit_path
    env_p = os.environ.get("CROSSLINK_LIB_PATH")
    if env_p and Path(env_p).is_file():
        return env_p

    sys_name = platform.system()
    lib_name = (
        "crosslink_core.dll"
        if sys_name == "Windows"
        else ("libcrosslink_core.dylib" if sys_name == "Darwin" else "libcrosslink_core.so")
    )
    here = Path(__file__).resolve().parent
    candidates = [
        here / lib_name,
        here.parent / "bin" / lib_name,
        here.parent / "lib" / lib_name,
        here.parents[1] / "build" / lib_name,
        here.parents[1] / "dist" / f"CrossLinkSDK-v{__version__}" / "bin" / lib_name,
        here.parents[1] / "dist" / f"CrossLinkSDK-v{__version__}" / "lib" / lib_name,
        Path("/usr/local/lib") / lib_name,
    ]
    for c in candidates:
        if c.is_file():
            return str(c)
    return lib_name


def _load_lib(lib_path: Optional[str] = None) -> ctypes.CDLL:
    lib = ctypes.CDLL(_find_library(lib_path))

    lib.cl_init.restype = ctypes.c_int
    lib.cl_shutdown.restype = None
    lib.cl_get_version_string.restype = ctypes.c_char_p
    lib.cl_get_cpu_simd_info.restype = ctypes.c_char_p
    lib.cl_free_string.argtypes = [ctypes.c_void_p]
    lib.cl_check_abi_compatibility.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.cl_check_abi_compatibility.restype = ctypes.c_int
    lib.cl_error_string.argtypes = [ctypes.c_int]
    lib.cl_error_string.restype = ctypes.c_char_p

    lib.cl_audio_bus_config_default.argtypes = [ctypes.POINTER(CL_AudioBusConfig)]
    lib.cl_sender_config_default.argtypes = [ctypes.POINTER(CL_SenderConfig)]
    lib.cl_receiver_config_default.argtypes = [ctypes.POINTER(CL_ReceiverConfig)]

    lib.cl_sender_create.restype = ctypes.c_void_p
    lib.cl_sender_destroy.argtypes = [ctypes.c_void_p]
    lib.cl_sender_configure_audio_bus.argtypes = [ctypes.c_void_p, ctypes.POINTER(CL_AudioBusConfig)]
    lib.cl_sender_configure_audio_bus.restype = ctypes.c_int
    lib.cl_sender_start.argtypes = [ctypes.c_void_p, ctypes.POINTER(CL_SenderConfig)]
    lib.cl_sender_start.restype = ctypes.c_int
    lib.cl_sender_stop.argtypes = [ctypes.c_void_p]
    lib.cl_sender_get_local_port.argtypes = [ctypes.c_void_p]
    lib.cl_sender_get_local_port.restype = ctypes.c_uint16
    lib.cl_sender_push_audio_channel.argtypes = [
        ctypes.c_void_p, ctypes.c_uint16, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int, ctypes.c_uint64,
    ]
    lib.cl_sender_push_audio_channel.restype = ctypes.c_int
    lib.cl_sender_push_audio_multichannel.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint32,
        ctypes.c_int, ctypes.c_int, ctypes.c_uint64,
    ]
    lib.cl_sender_push_audio_multichannel.restype = ctypes.c_int
    lib.cl_sender_set_audio_latency_ms.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.cl_sender_set_audio_latency_ms.restype = ctypes.c_int
    lib.cl_sender_set_audio_codec.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.cl_sender_set_audio_codec.restype = ctypes.c_int
    lib.cl_sender_set_audio_bitrate.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.cl_sender_set_audio_bitrate.restype = ctypes.c_int
    lib.cl_sender_set_wifi_redundancy.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_uint32]
    lib.cl_sender_set_wifi_redundancy.restype = ctypes.c_int
    lib.cl_sender_get_master_clock_samples.argtypes = [ctypes.c_void_p]
    lib.cl_sender_get_master_clock_samples.restype = ctypes.c_uint64
    lib.cl_sender_push_midi.argtypes = [
        ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint64,
    ]
    lib.cl_sender_push_midi.restype = ctypes.c_int
    lib.cl_sender_get_stream_stats.argtypes = [ctypes.c_void_p, ctypes.POINTER(CL_StreamStats)]
    lib.cl_sender_get_stream_stats.restype = ctypes.c_int

    lib.cl_receiver_create.restype = ctypes.c_void_p
    lib.cl_receiver_destroy.argtypes = [ctypes.c_void_p]
    lib.cl_receiver_configure_audio_bus.argtypes = [ctypes.c_void_p, ctypes.POINTER(CL_AudioBusConfig)]
    lib.cl_receiver_configure_audio_bus.restype = ctypes.c_int
    lib.cl_receiver_start.argtypes = [ctypes.c_void_p, ctypes.POINTER(CL_ReceiverConfig)]
    lib.cl_receiver_start.restype = ctypes.c_int
    lib.cl_receiver_stop.argtypes = [ctypes.c_void_p]
    lib.cl_receiver_get_local_port.argtypes = [ctypes.c_void_p]
    lib.cl_receiver_get_local_port.restype = ctypes.c_uint16
    lib.cl_receiver_set_audio_latency_ms.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.cl_receiver_set_audio_latency_ms.restype = ctypes.c_int
    lib.cl_receiver_set_adaptive_jitter.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.cl_receiver_set_adaptive_jitter.restype = ctypes.c_int
    lib.cl_receiver_get_effective_latency_ms.argtypes = [ctypes.c_void_p]
    lib.cl_receiver_get_effective_latency_ms.restype = ctypes.c_double
    lib.cl_receiver_set_multichannel_audio_callback.argtypes = [ctypes.c_void_p, MC_AUDIO_CB_TYPE, ctypes.c_void_p]
    lib.cl_receiver_set_multichannel_audio_callback.restype = ctypes.c_int
    lib.cl_receiver_set_midi_callback.argtypes = [ctypes.c_void_p, MIDI_CB_TYPE, ctypes.c_void_p]
    lib.cl_receiver_pop_audio_multichannel.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_float), ctypes.c_uint16, ctypes.c_uint32, ctypes.c_int,
    ]
    lib.cl_receiver_pop_audio_multichannel.restype = ctypes.c_int
    lib.cl_receiver_get_channel_stats.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.POINTER(CL_AudioChannelStats)]
    lib.cl_receiver_get_channel_stats.restype = ctypes.c_int
    lib.cl_receiver_get_stream_stats.argtypes = [ctypes.c_void_p, ctypes.POINTER(CL_StreamStats)]
    lib.cl_receiver_get_stream_stats.restype = ctypes.c_int

    lib.cl_discovery_scan_audio_nodes.argtypes = [
        ctypes.c_int, ctypes.c_uint16, ctypes.POINTER(CL_AudioNode), ctypes.c_int,
    ]
    lib.cl_discovery_scan_audio_nodes.restype = ctypes.c_int

    if not lib.cl_check_abi_compatibility(CL_SDK_VERSION_MAJOR, CL_SDK_VERSION_MINOR):
        raise RuntimeError("Incompatible CrossLinkSDK shared library ABI version (expected v1.x)")
    lib.cl_init()
    return lib


class CrossLinkSDK:
    """Entry point holding the loaded CrossLink Audio shared library."""

    def __init__(self, lib_path: Optional[str] = None) -> None:
        self.lib = _load_lib(lib_path)

    @property
    def version(self) -> str:
        return self.lib.cl_get_version_string().decode("utf-8")

    @property
    def simd(self) -> str:
        return self.lib.cl_get_cpu_simd_info().decode("utf-8")

    def discover_nodes(self, timeout_ms: int = 350, discovery_port: int = 0) -> List[dict]:
        buf = (CL_AudioNode * 32)()
        n = self.lib.cl_discovery_scan_audio_nodes(timeout_ms, discovery_port, buf, 32)
        return [
            {
                "ip": buf[i].ip.decode("utf-8"),
                "name": buf[i].name.decode("utf-8"),
                "hostname": buf[i].hostname.decode("utf-8"),
                "audio_driver": buf[i].audio_driver.decode("utf-8"),
                "rx_port": int(buf[i].rx_port),
                "tx_port": int(buf[i].tx_port),
                "channels": int(buf[i].channels),
                "duplex": bool(buf[i].duplex),
                "rtt_ms": int(buf[i].rtt_ms),
                "os": buf[i].os.decode("utf-8"),
            }
            for i in range(max(0, n))
        ]


class CrossLinkSender:
    """64-channel phase-coherent AoIP + MIDI 2.0 Sender."""

    def __init__(
        self,
        dest_ip: str = "127.0.0.1",
        dest_port: int = 5004,
        sample_rate: int = 48000,
        latency_ms: int = 20,
        audio_codec: int = 0,
        lib_path: Optional[str] = None,
    ) -> None:
        self._sdk = CrossLinkSDK(lib_path)
        self._lib = self._sdk.lib
        self._handle = self._lib.cl_sender_create()
        if not self._handle:
            raise MemoryError("cl_sender_create returned NULL")
        self._cfg = CL_SenderConfig()
        self._lib.cl_sender_config_default(ctypes.byref(self._cfg))
        self._cfg.dest_ip = dest_ip.encode("utf-8")
        self._cfg.dest_port = dest_port
        self._cfg.sample_rate = int(sample_rate)
        self._cfg.latency_ms = int(latency_ms)
        self._cfg.audio_codec = int(audio_codec)

    def configure_audio_bus(self, latency_ms: int = 20, sample_rate: int = 48000) -> None:
        bus = CL_AudioBusConfig()
        self._lib.cl_audio_bus_config_default(ctypes.byref(bus))
        bus.latency_ms = int(latency_ms)
        bus.sample_rate = int(sample_rate)
        rc = self._lib.cl_sender_configure_audio_bus(self._handle, ctypes.byref(bus))
        if rc != CL_OK:
            raise ValueError(self._lib.cl_error_string(rc).decode("utf-8"))

    def start(self) -> None:
        rc = self._lib.cl_sender_start(self._handle, ctypes.byref(self._cfg))
        if rc != CL_OK:
            raise RuntimeError(self._lib.cl_error_string(rc).decode("utf-8"))

    def stop(self) -> None:
        if self._handle:
            self._lib.cl_sender_stop(self._handle)

    def push_audio_channel(self, channel: int, samples, fmt: int = CL_AUDIO_FORMAT_F32,
                           media_ts: Optional[int] = None) -> None:
        ptr, _keep, n = _as_audio_buffer(samples, fmt)
        ts = CL_MEDIA_TS_AUTO if media_ts is None else int(media_ts)
        rc = self._lib.cl_sender_push_audio_channel(self._handle, int(channel), ptr, n, fmt, ts)
        if rc != CL_OK:
            raise RuntimeError(self._lib.cl_error_string(rc).decode("utf-8"))

    def push_audio_multichannel(self, frames, channels: Optional[int] = None,
                                fmt: int = CL_AUDIO_FORMAT_F32, media_ts: Optional[int] = None) -> None:
        if _HAS_NUMPY and isinstance(frames, np.ndarray) and frames.ndim == 2:
            n, ch = frames.shape
            ptr, _keep, _ = _as_audio_buffer(frames, fmt)
        else:
            if channels is None or channels <= 0:
                raise ValueError("channels is required when passing raw buffers")
            ptr, _keep, total = _as_audio_buffer(frames, fmt)
            ch = int(channels)
            n = total // ch
        ts = CL_MEDIA_TS_AUTO if media_ts is None else int(media_ts)
        rc = self._lib.cl_sender_push_audio_multichannel(
            self._handle, ptr, int(ch), int(n), fmt, CL_AUDIO_LAYOUT_INTERLEAVED, ts
        )
        if rc != CL_OK:
            raise RuntimeError(self._lib.cl_error_string(rc).decode("utf-8"))

    def push_midi(self, status: int, data1: int, data2: int, port: int = 0, media_ts_us: int = 0) -> None:
        rc = self._lib.cl_sender_push_midi(self._handle, status, data1, data2, port, media_ts_us)
        if rc != CL_OK:
            raise RuntimeError(self._lib.cl_error_string(rc).decode("utf-8"))

    def set_latency_ms(self, latency_ms: int) -> None:
        self._lib.cl_sender_set_audio_latency_ms(self._handle, int(latency_ms))

    def set_wifi_redundancy(self, enabled: bool, stagger_us: int = 1500) -> None:
        self._lib.cl_sender_set_wifi_redundancy(self._handle, 1 if enabled else 0, int(stagger_us))

    def master_clock_samples(self) -> int:
        return int(self._lib.cl_sender_get_master_clock_samples(self._handle))

    def stats(self) -> CL_StreamStats:
        s = CL_StreamStats()
        self._lib.cl_sender_get_stream_stats(self._handle, ctypes.byref(s))
        return s

    def close(self) -> None:
        if self._handle:
            self._lib.cl_sender_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> "CrossLinkSender":
        self.start()
        return self

    def __exit__(self, *_) -> None:
        self.close()


class CrossLinkReceiver:
    """64-channel phase-coherent AoIP + MIDI 2.0 Receiver."""

    def __init__(
        self,
        bind_port: int = 5004,
        sample_rate: int = 48000,
        latency_ms: int = 20,
        num_channels: int = 64,
        block_frames: int = 240,
        adaptive_jitter: bool = True,
        device_name: str = "PythonAudioNode",
        discovery_port: int = 5021,
        lib_path: Optional[str] = None,
    ) -> None:
        self._sdk = CrossLinkSDK(lib_path)
        self._lib = self._sdk.lib
        self._handle = self._lib.cl_receiver_create()
        if not self._handle:
            raise MemoryError("cl_receiver_create returned NULL")
        self._cfg = CL_ReceiverConfig()
        self._lib.cl_receiver_config_default(ctypes.byref(self._cfg))
        self._cfg.bind_port = bind_port
        self._cfg.discovery_port = discovery_port
        self._cfg.sample_rate = int(sample_rate)
        self._cfg.latency_ms = int(latency_ms)
        self._cfg.num_channels = int(num_channels)
        self._cfg.block_frames = int(block_frames)
        self._cfg.adaptive_jitter = 1 if adaptive_jitter else 0
        self._cfg.device_name = device_name.encode("utf-8")
        self._mc_cb_ref = None
        self._midi_cb_ref = None

    def start(self) -> int:
        rc = self._lib.cl_receiver_start(self._handle, ctypes.byref(self._cfg))
        if rc != CL_OK:
            raise RuntimeError(self._lib.cl_error_string(rc).decode("utf-8"))
        return int(self._lib.cl_receiver_get_local_port(self._handle))

    @property
    def local_port(self) -> int:
        return int(self._lib.cl_receiver_get_local_port(self._handle))

    def set_latency_ms(self, latency_ms: int) -> None:
        rc = self._lib.cl_receiver_set_audio_latency_ms(self._handle, int(latency_ms))
        if rc != CL_OK:
            raise ValueError(self._lib.cl_error_string(rc).decode("utf-8"))

    def set_adaptive_jitter(self, enabled: bool) -> None:
        rc = self._lib.cl_receiver_set_adaptive_jitter(self._handle, 1 if enabled else 0)
        if rc != CL_OK:
            raise ValueError(self._lib.cl_error_string(rc).decode("utf-8"))

    def effective_latency_ms(self) -> float:
        return float(self._lib.cl_receiver_get_effective_latency_ms(self._handle))

    def pop_audio_multichannel(self, num_channels: int, num_frames: int):
        ch = int(num_channels)
        nf = int(num_frames)
        buf = (ctypes.c_float * (ch * nf))()
        rc = self._lib.cl_receiver_pop_audio_multichannel(self._handle, buf, ch, nf, CL_AUDIO_LAYOUT_INTERLEAVED)
        if rc < 0:
            raise RuntimeError(self._lib.cl_error_string(rc).decode("utf-8"))
        got = min(rc, nf)
        if _HAS_NUMPY:
            return np.ctypeslib.as_array(buf)[: got * ch].reshape((got, ch)).copy()
        return [buf[i] for i in range(got * ch)]

    def on_multichannel_audio(self, callback: Optional[Callable[[object, int], None]]) -> None:
        if callback is None:
            self._mc_cb_ref = MC_AUDIO_CB_TYPE(0)
            self._lib.cl_receiver_set_multichannel_audio_callback(self._handle, self._mc_cb_ref, None)
            return

        def _trampoline(blk_ptr, _ud):
            if not blk_ptr:
                return
            b = blk_ptr.contents
            if not b.data or b.num_frames <= 0 or b.num_channels <= 0:
                return
            count = b.num_frames * b.num_channels
            if _HAS_NUMPY:
                arr = np.ctypeslib.as_array(b.data, shape=(count,)).reshape((b.num_frames, b.num_channels)).copy()
            else:
                arr = [b.data[i] for i in range(count)]
            callback(arr, int(b.playout_position_samples))

        self._mc_cb_ref = MC_AUDIO_CB_TYPE(_trampoline)
        self._lib.cl_receiver_set_multichannel_audio_callback(self._handle, self._mc_cb_ref, None)

    def on_midi(self, callback: Optional[Callable[[int, int, int, int, int], None]]) -> None:
        """callback(status, data1, data2, port, media_ts_us)"""
        if callback is None:
            self._midi_cb_ref = MIDI_CB_TYPE(0)
            self._lib.cl_receiver_set_midi_callback(self._handle, self._midi_cb_ref, None)
            return

        def _trampoline(ev_ptr, _ud):
            if not ev_ptr:
                return
            e = ev_ptr.contents
            callback(int(e.status), int(e.data1), int(e.data2), int(e.port), int(e.media_ts_us))

        self._midi_cb_ref = MIDI_CB_TYPE(_trampoline)
        self._lib.cl_receiver_set_midi_callback(self._handle, self._midi_cb_ref, None)

    def channel_stats(self, channel: int) -> CL_AudioChannelStats:
        st = CL_AudioChannelStats()
        self._lib.cl_receiver_get_channel_stats(self._handle, int(channel), ctypes.byref(st))
        return st

    def stats(self) -> CL_StreamStats:
        s = CL_StreamStats()
        self._lib.cl_receiver_get_stream_stats(self._handle, ctypes.byref(s))
        return s

    def stop(self) -> None:
        if self._handle:
            self._lib.cl_receiver_stop(self._handle)

    def close(self) -> None:
        if self._handle:
            self._lib.cl_receiver_set_multichannel_audio_callback(self._handle, MC_AUDIO_CB_TYPE(0), None)
            self._lib.cl_receiver_set_midi_callback(self._handle, MIDI_CB_TYPE(0), None)
            self._lib.cl_receiver_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> "CrossLinkReceiver":
        self.start()
        return self

    def __exit__(self, *_) -> None:
        self.close()
