"""Minimal ctypes binding to libopus (BSD-3-Clause), float API only.

Library lookup order: $OPUS_LIB, opus.dll / libopus.dylib next to this file, Homebrew
(/opt/homebrew/lib/libopus.dylib), then ctypes.util.find_library("opus").
On Windows the DLL is built from the official xiph tarball, see build_opus_win.bat.

opus_*_ctl is variadic. ctypes passes surplus arguments (beyond argtypes) as variadic
arguments (ffi_prep_cif_var), which is what the Apple arm64 ABI needs, so argtypes is
deliberately set to the two fixed parameters only.
"""
from __future__ import annotations

import ctypes
import ctypes.util
import os
import sys

import numpy as np

APPLICATION_VOIP = 2048
APPLICATION_AUDIO = 2049
APPLICATION_RESTRICTED_LOWDELAY = 2051
APPLICATIONS = {"voip": APPLICATION_VOIP, "audio": APPLICATION_AUDIO,
                "lowdelay": APPLICATION_RESTRICTED_LOWDELAY}

SET_BITRATE = 4002
GET_BITRATE = 4003
SET_VBR = 4006
SET_COMPLEXITY = 4010
SET_INBAND_FEC = 4012
SET_PACKET_LOSS_PERC = 4014
SET_DTX = 4016
SET_SIGNAL = 4024
GET_LOOKAHEAD = 4027
SIGNAL_VOICE = 3001
SIGNAL_MUSIC = 3002

MAX_PACKET = 1275 * 3


def _load() -> ctypes.CDLL:
    here = os.path.dirname(os.path.abspath(__file__))
    cands = []
    if os.environ.get("OPUS_LIB"):
        cands.append(os.environ["OPUS_LIB"])
    if sys.platform == "win32":
        cands += [os.path.join(here, "opus.dll"), "opus.dll"]
    else:
        cands += [os.path.join(here, "libopus.dylib"), "/opt/homebrew/lib/libopus.dylib",
                  "/usr/local/lib/libopus.dylib", "libopus.so.0"]
    found = ctypes.util.find_library("opus")
    if found:
        cands.append(found)
    err = None
    for c in cands:
        try:
            return ctypes.CDLL(c)
        except OSError as e:  # try the next one
            err = e
    raise OSError(f"libopus not found (tried {cands}): {err}")


_lib = _load()
_c_int_p = ctypes.POINTER(ctypes.c_int)
_lib.opus_get_version_string.restype = ctypes.c_char_p
_lib.opus_encoder_create.restype = ctypes.c_void_p
_lib.opus_encoder_create.argtypes = [ctypes.c_int32, ctypes.c_int, ctypes.c_int, _c_int_p]
_lib.opus_encoder_destroy.argtypes = [ctypes.c_void_p]
_lib.opus_encode_float.restype = ctypes.c_int32
_lib.opus_encode_float.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int,
                                   ctypes.c_char_p, ctypes.c_int32]
_lib.opus_encoder_ctl.restype = ctypes.c_int
_lib.opus_encoder_ctl.argtypes = [ctypes.c_void_p, ctypes.c_int]
_lib.opus_decoder_create.restype = ctypes.c_void_p
_lib.opus_decoder_create.argtypes = [ctypes.c_int32, ctypes.c_int, _c_int_p]
_lib.opus_decoder_destroy.argtypes = [ctypes.c_void_p]
_lib.opus_decode_float.restype = ctypes.c_int
_lib.opus_decode_float.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int32,
                                   ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
_lib.opus_strerror.restype = ctypes.c_char_p
_lib.opus_strerror.argtypes = [ctypes.c_int]


_lib.opus_packet_has_lbrr.restype = ctypes.c_int
_lib.opus_packet_has_lbrr.argtypes = [ctypes.c_char_p, ctypes.c_int32]


def packet_has_lbrr(data: bytes) -> bool:
    """True if the packet carries in-band FEC (LBRR) for the previous frame (opus >= 1.5)."""
    return _lib.opus_packet_has_lbrr(data, len(data)) > 0


def version() -> str:
    return _lib.opus_get_version_string().decode()


class OpusError(RuntimeError):
    def __init__(self, code: int, what: str):
        super().__init__(f"{what}: {_lib.opus_strerror(code).decode()} ({code})")


class Encoder:
    def __init__(self, rate: int = 48000, channels: int = 1, application: str = "voip",
                 bitrate: int = 32000, fec: bool = True, loss_perc: int = 10,
                 dtx: bool = False, complexity: int = 9, signal: str | None = None):
        err = ctypes.c_int(0)
        self._st = _lib.opus_encoder_create(rate, channels, APPLICATIONS[application],
                                            ctypes.byref(err))
        if err.value != 0 or not self._st:
            raise OpusError(err.value, "opus_encoder_create")
        self.channels = channels
        self._buf = ctypes.create_string_buffer(MAX_PACKET)
        self.ctl(SET_BITRATE, ctypes.c_int(bitrate))
        self.ctl(SET_COMPLEXITY, ctypes.c_int(complexity))
        self.ctl(SET_INBAND_FEC, ctypes.c_int(1 if fec else 0))
        self.ctl(SET_PACKET_LOSS_PERC, ctypes.c_int(loss_perc if fec else 0))
        self.ctl(SET_DTX, ctypes.c_int(1 if dtx else 0))
        if signal:
            self.ctl(SET_SIGNAL, ctypes.c_int(SIGNAL_VOICE if signal == "voice" else SIGNAL_MUSIC))

    def ctl(self, req: int, *args) -> None:
        r = _lib.opus_encoder_ctl(self._st, req, *args)
        if r != 0:
            raise OpusError(r, f"opus_encoder_ctl({req})")

    def get(self, req: int) -> int:
        v = ctypes.c_int(0)
        self.ctl(req, ctypes.byref(v))
        return v.value

    def encode(self, pcm: np.ndarray) -> bytes:
        """pcm: float32 C-contiguous (frames, channels)."""
        pcm = np.ascontiguousarray(pcm, dtype=np.float32)
        n = _lib.opus_encode_float(self._st, pcm.ctypes.data, pcm.shape[0], self._buf, MAX_PACKET)
        if n < 0:
            raise OpusError(n, "opus_encode_float")
        return self._buf.raw[:n]

    def __del__(self):
        if getattr(self, "_st", None):
            _lib.opus_encoder_destroy(self._st)
            self._st = None


class Decoder:
    def __init__(self, rate: int = 48000, channels: int = 1):
        err = ctypes.c_int(0)
        self._st = _lib.opus_decoder_create(rate, channels, ctypes.byref(err))
        if err.value != 0 or not self._st:
            raise OpusError(err.value, "opus_decoder_create")
        self.channels = channels

    def decode(self, data: bytes | None, frame_size: int, fec: bool = False) -> np.ndarray:
        """data=None -> packet loss concealment for frame_size samples.
        fec=True with the *next* packet -> recover the lost previous frame from its LBRR data."""
        out = np.empty((frame_size, self.channels), dtype=np.float32)
        n = _lib.opus_decode_float(self._st, data, len(data) if data else 0, out.ctypes.data,
                                   frame_size, 1 if fec else 0)
        if n < 0:
            raise OpusError(n, "opus_decode_float")
        return out[:n]

    def __del__(self):
        if getattr(self, "_st", None):
            _lib.opus_decoder_destroy(self._st)
            self._st = None
