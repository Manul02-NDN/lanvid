"""
LanVid SDK — Python ctypes + optional NumPy Bindings (Windows / Linux / macOS)

Zero required third-party dependencies. If `numpy` is installed, frames can be sent and
received directly as `(H, W, 4)` uint8 `numpy.ndarray` objects with zero extra copies.
"""

from __future__ import annotations

import ctypes
import os
import platform
from pathlib import Path
from typing import Callable, List, Optional, Tuple

try:
    import numpy as np  # type: ignore
    _HAS_NUMPY = True
except ImportError:
    np = None  # type: ignore
    _HAS_NUMPY = False

# ---- Constants matching sdk/include/lanvid/lanvid_types.h ----
LV_SDK_VERSION_MAJOR = 1
LV_SDK_VERSION_MINOR = 0

LV_PIXEL_FORMAT_BGRA8888 = 0
LV_PIXEL_FORMAT_RGBA8888 = 1
LV_PIXEL_FORMAT_I420 = 2
LV_PIXEL_FORMAT_NV12 = 3
LV_PIXEL_FORMAT_NV21 = 4

LV_AUDIO_FORMAT_PCM16 = 0
LV_AUDIO_FORMAT_F32 = 1

LV_OK = 0


class LV_VideoFrameView(ctypes.Structure):
    _fields_ = [
        ("data", ctypes.POINTER(ctypes.c_uint8)),
        ("size", ctypes.c_size_t),
        ("width", ctypes.c_int32),
        ("height", ctypes.c_int32),
        ("stride", ctypes.c_int32),
        ("pixel_format", ctypes.c_int32),
        ("frame_id", ctypes.c_uint32),
        ("timestamp_us", ctypes.c_uint32),
        ("slices_ok", ctypes.c_uint16),
        ("slices_total", ctypes.c_uint16),
        ("quality", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8 * 3),
    ]


class LV_AudioChunkView(ctypes.Structure):
    _fields_ = [
        ("data", ctypes.c_void_p),
        ("num_frames", ctypes.c_int32),
        ("sample_rate", ctypes.c_int32),
        ("channels", ctypes.c_int32),
        ("audio_format", ctypes.c_int32),
        ("seq_id", ctypes.c_uint32),
        ("timestamp_us", ctypes.c_uint32),
    ]


class LV_YuvPlanes(ctypes.Structure):
    _fields_ = [
        ("plane_y", ctypes.POINTER(ctypes.c_uint8)),
        ("plane_u", ctypes.POINTER(ctypes.c_uint8)),
        ("plane_v", ctypes.POINTER(ctypes.c_uint8)),
        ("y_stride", ctypes.c_int32),
        ("u_stride", ctypes.c_int32),
        ("v_stride", ctypes.c_int32),
        ("uv_pixel_stride", ctypes.c_int32),
        ("pixel_format", ctypes.c_int32),
    ]


class LV_SenderConfig(ctypes.Structure):
    _fields_ = [
        ("dest_ip", ctypes.c_char * 64),
        ("dest_port", ctypes.c_uint16),
        ("reserved_port", ctypes.c_uint16),
        ("width", ctypes.c_int32),
        ("height", ctypes.c_int32),
        ("fps", ctypes.c_double),
        ("quality", ctypes.c_int32),
        ("chroma", ctypes.c_int32),
        ("slice_height", ctypes.c_int32),
        ("mtu", ctypes.c_int32),
        ("enable_crc", ctypes.c_int32),
        ("fec_group_size", ctypes.c_int32),
        ("adaptive_quality", ctypes.c_int32),
        ("q_min", ctypes.c_int32),
        ("q_max", ctypes.c_int32),
        ("threads", ctypes.c_int32),
        ("pace_mbps", ctypes.c_double),
    ]


class LV_ReceiverConfig(ctypes.Structure):
    _fields_ = [
        ("bind_ip", ctypes.c_char * 64),
        ("bind_port", ctypes.c_uint16),
        ("discovery_port", ctypes.c_uint16),
        ("output_format", ctypes.c_int32),
        ("decode_threads", ctypes.c_int32),
        ("conceal_mode", ctypes.c_int32),
        ("frame_timeout_ms", ctypes.c_double),
        ("audio_target_ms", ctypes.c_double),
        ("enable_os_audio", ctypes.c_int32),
        ("enable_discovery", ctypes.c_int32),
        ("device_name", ctypes.c_char * 128),
    ]


class LV_StreamStats(ctypes.Structure):
    _fields_ = [
        ("state", ctypes.c_int32),
        ("width", ctypes.c_int32),
        ("height", ctypes.c_int32),
        ("active_quality", ctypes.c_int32),
        ("fps", ctypes.c_double),
        ("bitrate_mbps", ctypes.c_double),
        ("latency_p50_ms", ctypes.c_double),
        ("latency_p99_ms", ctypes.c_double),
        ("encode_ms", ctypes.c_double),
        ("packet_loss_pct", ctypes.c_double),
        ("frames_total", ctypes.c_uint64),
        ("frames_concealed", ctypes.c_uint64),
        ("packets_total", ctypes.c_uint64),
        ("fec_recovered", ctypes.c_uint64),
        ("crc_errors", ctypes.c_uint64),
        ("late_packets", ctypes.c_uint64),
        ("audio_packets", ctypes.c_uint64),
        ("audio_kbps", ctypes.c_double),
        ("audio_buffer_ms", ctypes.c_double),
    ]


class LV_DiscoveredPeer(ctypes.Structure):
    _fields_ = [
        ("ip", ctypes.c_char * 64),
        ("port", ctypes.c_uint16),
        ("reserved", ctypes.c_uint16),
        ("rtt_ms", ctypes.c_uint32),
        ("name", ctypes.c_char * 128),
        ("os", ctypes.c_char * 32),
        ("simd", ctypes.c_char * 32),
    ]


VIDEO_CB_TYPE = ctypes.CFUNCTYPE(None, ctypes.POINTER(LV_VideoFrameView), ctypes.c_void_p)
AUDIO_CB_TYPE = ctypes.CFUNCTYPE(None, ctypes.POINTER(LV_AudioChunkView), ctypes.c_void_p)
STATE_CB_TYPE = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p)


def _find_library(explicit_path: Optional[str] = None) -> str:
    if explicit_path:
        return explicit_path
    env_p = os.environ.get("LANVID_LIB_PATH")
    if env_p and Path(env_p).is_file():
        return env_p

    sys_name = platform.system()
    lib_name = (
        "lanvid_core.dll"
        if sys_name == "Windows"
        else ("liblanvid_core.dylib" if sys_name == "Darwin" else "liblanvid_core.so")
    )
    here = Path(__file__).resolve().parent
    candidates = [
        here / lib_name,
        here.parent / "bin" / lib_name,
        here.parent / "lib" / lib_name,
        here.parents[1] / "build" / lib_name,
        here.parents[1] / "dist" / "LanVidSDK-v1.0.0" / "bin" / lib_name,
        here.parents[1] / "dist" / "LanVidSDK-v1.0.0" / "lib" / lib_name,
    ]
    for c in candidates:
        if c.is_file():
            return str(c)
    return lib_name


def _load_lib(lib_path: Optional[str] = None) -> ctypes.CDLL:
    lib = ctypes.CDLL(_find_library(lib_path))

    lib.lv_init.restype = ctypes.c_int
    lib.lv_shutdown.restype = None
    lib.lv_get_version_string.restype = ctypes.c_char_p
    lib.lv_get_cpu_simd_info.restype = ctypes.c_char_p
    lib.lv_check_abi_compatibility.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.lv_check_abi_compatibility.restype = ctypes.c_int
    lib.lv_error_string.argtypes = [ctypes.c_int]
    lib.lv_error_string.restype = ctypes.c_char_p

    lib.lv_sdk_sender_config_default.argtypes = [ctypes.POINTER(LV_SenderConfig)]
    lib.lv_sdk_receiver_config_default.argtypes = [ctypes.POINTER(LV_ReceiverConfig)]

    lib.lv_sender_create.restype = ctypes.c_void_p
    lib.lv_sender_destroy.argtypes = [ctypes.c_void_p]
    lib.lv_sdk_sender_start.argtypes = [ctypes.c_void_p, ctypes.POINTER(LV_SenderConfig)]
    lib.lv_sdk_sender_start.restype = ctypes.c_int
    lib.lv_sender_stop.argtypes = [ctypes.c_void_p]
    lib.lv_sender_push_packed_frame.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
        ctypes.c_int, ctypes.c_int, ctypes.c_uint32,
    ]
    lib.lv_sender_push_packed_frame.restype = ctypes.c_int
    lib.lv_sender_push_audio_chunk.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
        ctypes.c_int, ctypes.c_int, ctypes.c_uint32,
    ]
    lib.lv_sender_push_audio_chunk.restype = ctypes.c_int
    lib.lv_sender_request_keyframe_quality.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.lv_sender_request_keyframe_quality.restype = ctypes.c_int
    lib.lv_sender_set_fec_group.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.lv_sender_set_fec_group.restype = ctypes.c_int
    lib.lv_sender_get_stream_stats.argtypes = [ctypes.c_void_p, ctypes.POINTER(LV_StreamStats)]
    lib.lv_sender_get_stream_stats.restype = ctypes.c_int

    lib.lv_receiver_create.restype = ctypes.c_void_p
    lib.lv_receiver_destroy.argtypes = [ctypes.c_void_p]
    lib.lv_sdk_receiver_start.argtypes = [ctypes.c_void_p, ctypes.POINTER(LV_ReceiverConfig)]
    lib.lv_sdk_receiver_start.restype = ctypes.c_int
    lib.lv_receiver_stop.argtypes = [ctypes.c_void_p]
    lib.lv_receiver_get_local_port.argtypes = [ctypes.c_void_p]
    lib.lv_receiver_get_local_port.restype = ctypes.c_uint16
    lib.lv_receiver_set_video_callback.argtypes = [ctypes.c_void_p, VIDEO_CB_TYPE, ctypes.c_void_p]
    lib.lv_receiver_set_audio_callback.argtypes = [ctypes.c_void_p, AUDIO_CB_TYPE, ctypes.c_void_p]
    lib.lv_receiver_set_state_callback.argtypes = [ctypes.c_void_p, STATE_CB_TYPE, ctypes.c_void_p]
    lib.lv_receiver_get_latest_frame.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_uint32), ctypes.c_int,
    ]
    lib.lv_receiver_get_latest_frame.restype = ctypes.c_int
    lib.lv_receiver_set_volume.argtypes = [ctypes.c_void_p, ctypes.c_float]
    lib.lv_receiver_get_stream_stats.argtypes = [ctypes.c_void_p, ctypes.POINTER(LV_StreamStats)]
    lib.lv_receiver_get_stream_stats.restype = ctypes.c_int

    lib.lv_discovery_scan_peers.argtypes = [
        ctypes.c_int, ctypes.c_uint16, ctypes.POINTER(LV_DiscoveredPeer), ctypes.c_int,
    ]
    lib.lv_discovery_scan_peers.restype = ctypes.c_int

    if not lib.lv_check_abi_compatibility(LV_SDK_VERSION_MAJOR, LV_SDK_VERSION_MINOR):
        raise RuntimeError("Incompatible LanVidSDK shared library ABI version")
    lib.lv_init()
    return lib


class LanVidSDK:
    """Entry point holding the loaded shared library."""

    def __init__(self, lib_path: Optional[str] = None) -> None:
        self.lib = _load_lib(lib_path)

    @property
    def version(self) -> str:
        return self.lib.lv_get_version_string().decode("utf-8")

    @property
    def simd(self) -> str:
        return self.lib.lv_get_cpu_simd_info().decode("utf-8")

    def discover_peers(self, timeout_ms: int = 350, discovery_port: int = 0) -> List[dict]:
        buf = (LV_DiscoveredPeer * 32)()
        n = self.lib.lv_discovery_scan_peers(timeout_ms, discovery_port, buf, 32)
        return [
            {
                "ip": buf[i].ip.decode("utf-8"),
                "port": int(buf[i].port),
                "rtt_ms": int(buf[i].rtt_ms),
                "name": buf[i].name.decode("utf-8"),
                "os": buf[i].os.decode("utf-8"),
                "simd": buf[i].simd.decode("utf-8"),
            }
            for i in range(max(0, n))
        ]


class LanVidSender:
    def __init__(
        self,
        dest_ip: str = "127.0.0.1",
        dest_port: int = 5000,
        quality: int = 80,
        fec_group: int = 6,
        adaptive: bool = False,
        lib_path: Optional[str] = None,
    ) -> None:
        self._sdk = LanVidSDK(lib_path)
        self._lib = self._sdk.lib
        self._handle = self._lib.lv_sender_create()
        self._cfg = LV_SenderConfig()
        self._lib.lv_sdk_sender_config_default(ctypes.byref(self._cfg))
        self._cfg.dest_ip = dest_ip.encode("utf-8")
        self._cfg.dest_port = dest_port
        self._cfg.quality = quality
        self._cfg.fec_group_size = fec_group
        self._cfg.adaptive_quality = 1 if adaptive else 0

    def start(self) -> None:
        rc = self._lib.lv_sdk_sender_start(self._handle, ctypes.byref(self._cfg))
        if rc != LV_OK:
            raise RuntimeError(self._lib.lv_error_string(rc).decode("utf-8"))

    def stop(self) -> None:
        if self._handle:
            self._lib.lv_sender_stop(self._handle)

    def push_frame(self, frame, width: Optional[int] = None, height: Optional[int] = None,
                   pixel_format: int = LV_PIXEL_FORMAT_BGRA8888, timestamp_us: int = 0) -> None:
        """Accepts a `(H, W, 4)` uint8 numpy.ndarray OR raw bytes/bytearray."""
        if _HAS_NUMPY and isinstance(frame, np.ndarray):
            arr = np.ascontiguousarray(frame, dtype=np.uint8)
            h, w = arr.shape[0], arr.shape[1]
            stride = arr.strides[0]
            ptr = arr.ctypes.data_as(ctypes.c_void_p)
        else:
            if width is None or height is None:
                raise ValueError("width and height are required when passing raw bytes")
            w, h = width, height
            stride = w * 4
            buf = (ctypes.c_uint8 * len(frame)).from_buffer_copy(frame) if isinstance(frame, bytes) else (ctypes.c_uint8 * len(frame)).from_buffer(frame)
            ptr = ctypes.cast(buf, ctypes.c_void_p)
        rc = self._lib.lv_sender_push_packed_frame(self._handle, ptr, w, h, stride, pixel_format, timestamp_us)
        if rc != LV_OK:
            raise RuntimeError(self._lib.lv_error_string(rc).decode("utf-8"))

    def set_quality(self, quality: int) -> None:
        self._lib.lv_sender_request_keyframe_quality(self._handle, int(quality))

    def set_fec_group(self, group_size: int) -> None:
        self._lib.lv_sender_set_fec_group(self._handle, int(group_size))

    def stats(self) -> LV_StreamStats:
        s = LV_StreamStats()
        self._lib.lv_sender_get_stream_stats(self._handle, ctypes.byref(s))
        return s

    def close(self) -> None:
        if self._handle:
            self._lib.lv_sender_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> "LanVidSender":
        self.start()
        return self

    def __exit__(self, *_) -> None:
        self.close()


class LanVidReceiver:
    def __init__(
        self,
        bind_port: int = 5000,
        output_format: int = LV_PIXEL_FORMAT_BGRA8888,
        device_name: str = "PythonReceiver",
        discovery_port: int = 5099,
        lib_path: Optional[str] = None,
    ) -> None:
        self._sdk = LanVidSDK(lib_path)
        self._lib = self._sdk.lib
        self._handle = self._lib.lv_receiver_create()
        self._cfg = LV_ReceiverConfig()
        self._lib.lv_sdk_receiver_config_default(ctypes.byref(self._cfg))
        self._cfg.bind_port = bind_port
        self._cfg.discovery_port = discovery_port
        self._cfg.output_format = output_format
        self._cfg.device_name = device_name.encode("utf-8")
        self._video_cb_ref = None
        self._last_seq = ctypes.c_uint32(0)
        self._poll_buf: Optional[bytearray] = None

    def on_video(self, callback: Callable[[object, int, int, int], None]) -> None:
        """Registers callback(frame_ndarray_or_bytes, width, height, frame_id)."""
        def _trampoline(view_ptr, _ud):
            v = view_ptr.contents
            raw = ctypes.string_at(v.data, v.size)
            if _HAS_NUMPY:
                arr = np.frombuffer(raw, dtype=np.uint8).reshape((v.height, v.width, 4)).copy()
                callback(arr, v.width, v.height, v.frame_id)
            else:
                callback(raw, v.width, v.height, v.frame_id)

        self._video_cb_ref = VIDEO_CB_TYPE(_trampoline)
        self._lib.lv_receiver_set_video_callback(self._handle, self._video_cb_ref, None)

    def start(self) -> int:
        rc = self._lib.lv_sdk_receiver_start(self._handle, ctypes.byref(self._cfg))
        if rc != LV_OK:
            raise RuntimeError(self._lib.lv_error_string(rc).decode("utf-8"))
        return int(self._lib.lv_receiver_get_local_port(self._handle))

    @property
    def local_port(self) -> int:
        return int(self._lib.lv_receiver_get_local_port(self._handle))

    def get_latest_frame(self, want_rgba: bool = False) -> Optional[Tuple[object, int, int]]:
        w = ctypes.c_int(0)
        h = ctypes.c_int(0)
        buf_len = len(self._poll_buf) if self._poll_buf is not None else 0
        ptr = (ctypes.c_uint8 * buf_len).from_buffer(self._poll_buf) if buf_len else None
        rc = self._lib.lv_receiver_get_latest_frame(
            self._handle, ptr, buf_len, ctypes.byref(w), ctypes.byref(h),
            ctypes.byref(self._last_seq), 1 if want_rgba else 0,
        )
        if rc == -1 and w.value > 0 and h.value > 0:
            self._poll_buf = bytearray(w.value * h.value * 4)
            ptr = (ctypes.c_uint8 * len(self._poll_buf)).from_buffer(self._poll_buf)
            rc = self._lib.lv_receiver_get_latest_frame(
                self._handle, ptr, len(self._poll_buf), ctypes.byref(w), ctypes.byref(h),
                ctypes.byref(self._last_seq), 1 if want_rgba else 0,
            )
        if rc != 1 or self._poll_buf is None:
            return None
        if _HAS_NUMPY:
            arr = np.frombuffer(self._poll_buf, dtype=np.uint8).reshape((h.value, w.value, 4)).copy()
            return arr, w.value, h.value
        return bytes(self._poll_buf), w.value, h.value

    def stats(self) -> LV_StreamStats:
        s = LV_StreamStats()
        self._lib.lv_receiver_get_stream_stats(self._handle, ctypes.byref(s))
        return s

    def stop(self) -> None:
        if self._handle:
            self._lib.lv_receiver_stop(self._handle)

    def close(self) -> None:
        if self._handle:
            self._lib.lv_receiver_set_video_callback(self._handle, VIDEO_CB_TYPE(0), None)
            self._lib.lv_receiver_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> "LanVidReceiver":
        self.start()
        return self

    def __exit__(self, *_) -> None:
        self.close()
