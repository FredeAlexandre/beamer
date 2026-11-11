# SPDX-License-Identifier: GPL-3.0-only
"""The `camera_*` messages on the Beamer link (dev/camera/PHASE2.md §5): builders and validators.

Pure. A message is the link's `{"type", "data"}` object. `read` returns the validated data of a
`camera_*` message, or None for anything malformed: a malformed camera message is ignored, it never
ends a link (§5.1). Unknown fields are ignored and dropped (except the route's unknown top-level
entries, which are kept and forwarded, §6.2).

No new cap: camera messages need cap `audio` on both ends and the feature word `camera1` in the
peer's `audio_hello.features` (§5.1); `may_send` is that rule.
"""

import base64
import binascii
import json
import re
import time

from core import protocol, settings_sync
from core.camera import model

FEATURE = "camera1"           # audio_hello.features word (§5.1)
VIDEO_FEATURE = "video1"      # helper ready.features word: the engine has the video plane (§3.1)
SYNTH_FEATURE = "camera_synth"

DEVICES = "camera_devices"
ROUTE = "camera_route"
OPEN = "camera_open"
OPENED = "camera_opened"
CLOSE = "camera_close"
TYPES = (DEVICES, ROUTE, OPEN, OPENED, CLOSE)
FLOODED = (DEVICES, ROUTE)

MAX_SIZE = 16 * 1024
MAX_CAMERAS = 16
MAX_NATIVE = 16
MAX_KEY = 200
MAX_NAME = 128
MAX_APP = 128
MAX_EFFECTS = 16
MAX_EXTRA = 8
MAX_STREAM = 2 ** 31 - 1
KEY_SIZE = 32
OPEN_ERRORS = ("not_installed", "outdated", "helper", "unsupported", "not_source")
CLOSE_REASONS = ("route", "demand", "permission", "sharing_off", "camera_gone", "helper", "shutdown",
                 "no_receiver", "error")
PROFILES = ("constrained_high", "high", "main", "baseline", "constrained_baseline")
FLOW_SHAPE = re.compile(r"[0-9a-f]{8}")
NATIVE_SHAPE = re.compile(r"[1-9][0-9]{0,4}x[1-9][0-9]{0,4}@[0-9]{1,3}(\.[0-9]{1,3})?")
TEST_SOURCE = re.compile(r"synth(:[1-9][0-9]{1,4}x[1-9][0-9]{1,4})?(;start_ms=[0-9]{1,5})?")
_WORD = re.compile(r"[A-Za-z0-9_.:-]{1,32}")
_CODE = re.compile(r"[a-z_]{1,32}")


def message(kind, data):
    return {"type": kind, "data": data}


def size_ok(msg):
    """Whether a message fits the 16 KiB limit (§5.1) once encoded as the link encodes it."""
    return len(json.dumps(msg, separators=(",", ":")).encode("utf-8")) < MAX_SIZE


def is_camera(msg):
    return isinstance(msg, dict) and isinstance(msg.get("type"), str) and msg["type"].startswith("camera_")


def may_send(caps, hello_features) -> bool:
    """Whether a `camera_*` message may be posted to a peer: cap `audio` and `camera1` in its hello."""
    return "audio" in (caps or ()) and FEATURE in (hello_features or ())


def announce(camera_flag: bool, helper_features) -> bool:
    """Whether this machine announces `camera1` (§5.1): the flag is on and the helper has `video1`."""
    return bool(camera_flag) and VIDEO_FEATURE in (helper_features or ())


def codec(w, h, bitrate, fps=model.FPS):
    """The `codec` object of `send` and `camera_open` (§4.3)."""
    return {"name": "h264", "profile": "constrained_high", "w": int(w), "h": int(h), "fps": int(fps),
            "bitrate": int(bitrate), "mtu": 1200, "ltr": True}


# Builders ---------------------------------------------------------------------------------------

def devices(machine):
    return message(DEVICES, model.machine_data(machine))


def route(value):
    return message(ROUTE, model.route_data(value))


def open_stream(stream, flow, codec_data, key, camera, stamp):
    return message(OPEN, {"stream": stream, "flow": flow, "codec": dict(codec_data), "key": key,
                          "camera": model.ref_data(camera), "for": model.stamp_data(stamp)})


def opened(stream, ok, error=None):
    return message(OPENED, {"stream": stream, "ok": bool(ok), "error": None if ok else error})


def close(stream, reason):
    return message(CLOSE, {"stream": stream, "reason": reason})


# Validators -------------------------------------------------------------------------------------

def _int(value, low, high):
    return type(value) is int and low <= value <= high


def _text(value, high, low=0):
    return isinstance(value, str) and low <= len(value) <= high


def _machine(value):
    return protocol.read_id(value) is not None


def _key_b64(value):
    if not isinstance(value, str) or len(value) > 64:
        return False
    try:
        return len(base64.b64decode(value, validate=True)) == KEY_SIZE
    except (binascii.Error, ValueError):
        return False


def _camera_key(value):
    return _text(value, MAX_KEY, 1)


def _stamp(data, now):
    if not isinstance(data, dict):
        return None
    set_at, by = data.get("set_at"), data.get("by", "")
    if not _int(set_at, 0, settings_sync.MAX_STAMP) or set_at > now + settings_sync.DAY:
        return None
    if by != "" and not _machine(by):
        return None
    return model.Stamp(set_at, by)


def _ref(value):
    """A CameraRef, None for null; raises ValueError when malformed."""
    if value is None:
        return None
    if not isinstance(value, dict) or not _camera_key(value.get("key")) or not _text(value.get("name"), MAX_NAME):
        raise ValueError("bad camera ref")
    return model.CameraRef(value["key"], value["name"])


def _words(value, limit=32):
    if not isinstance(value, list) or len(value) > limit:
        return None
    return frozenset(word for word in value if isinstance(word, str) and _WORD.fullmatch(word))


def test_source_ok(value) -> bool:
    """A dev synthetic source spec (§7.4): "synth", "synth:WxH", "synth;start_ms=N", or both."""
    return isinstance(value, str) and TEST_SOURCE.fullmatch(value) is not None


def _cameras(entries, preferred):
    if not isinstance(entries, list) or len(entries) > MAX_CAMERAS:
        return None
    result, seen = [], set()
    for entry in entries:
        if not isinstance(entry, dict) or not _camera_key(entry.get("key")) or not _text(entry.get("name"), MAX_NAME):
            return None
        if entry["key"] in seen:
            continue
        seen.add(entry["key"])
        native = entry.get("native", [])
        if not isinstance(native, list) or len(native) > MAX_NATIVE:
            return None
        transport = entry.get("transport")
        result.append(model.Camera(entry["key"], entry["name"],
                                   transport if transport in model.TRANSPORTS else "other",
                                   tuple(value for value in native
                                         if isinstance(value, str) and NATIVE_SHAPE.fullmatch(value)),
                                   entry["key"] == preferred))
    return tuple(result)


def _capture(data):
    if data is None:
        return model.IDLE_CAPTURE
    if not isinstance(data, dict):
        return None
    effects = data.get("effects", [])
    if not isinstance(effects, list) or len(effects) > MAX_EFFECTS:
        return None
    state, error = data.get("state"), data.get("error")
    return model.Capture(state if state in model.CAPTURE_STATES else "idle", data.get("by_other") is True,
                         tuple(effect for effect in effects if effect in model.EFFECTS),
                         error if isinstance(error, str) and _CODE.fullmatch(error) else None)


def _want(value):
    if value is None:
        return None
    if (not isinstance(value, dict) or not _int(value.get("w"), 1, 8192) or not _int(value.get("h"), 1, 8192)
            or not _int(value.get("fps"), 1, 240)):
        raise ValueError("bad want")
    return (value["w"], value["h"], value["fps"])


def _virtual_camera(data):
    if data is None:
        return model.NO_VIRTUAL_CAMERA
    if not isinstance(data, dict):
        return None
    app = data.get("app")
    if app is not None and not _text(app, MAX_APP):
        return None
    install = data.get("install")
    return model.VirtualCamera(install if install in model.INSTALL_STATES else "broken",
                               data.get("present") is True, data.get("in_use") is True, app or None,
                               _want(data.get("want")))


def read_devices(data, now=None):
    """The MachineCamera a `camera_devices` message describes (sanitised, §7.2), or None."""
    now = time.time() if now is None else now
    if not isinstance(data, dict) or not _machine(data.get("machine")):
        return None
    if not _int(data.get("rev"), 0, settings_sync.MAX_STAMP) or data["rev"] > (now + settings_sync.DAY) * 1000:
        return None
    preferred = data.get("preferred")
    if preferred is not None and not _camera_key(preferred):
        return None
    cameras = _cameras(data.get("cameras", []), preferred)
    features = _words(data.get("features", []))
    capture = _capture(data.get("capture"))
    vcam = _virtual_camera(data.get("virtual_camera"))
    if cameras is None or features is None or capture is None or vcam is None:
        return None
    sharing = data.get("sharing")
    if sharing is not None and not isinstance(sharing, bool):
        return None
    permission, status, test_source = data.get("permission"), data.get("status"), data.get("test_source")
    machine = model.MachineCamera(
        id=data["machine"], platform=data["platform"] if _text(data.get("platform"), 16) else "",
        rev=data["rev"], status=status if status in model.STATUSES else "failed",
        features=frozenset(word for word in features if word in model.FEATURES), cameras=cameras,
        preferred=preferred, permission=permission if permission in model.PERMISSIONS else None,
        sharing=sharing, capture=capture, virtual_camera=vcam,
        test_source=test_source if test_source_ok(test_source) else None)
    return model.sanitize(machine)


def read_route(data, now=None):
    """The CameraRoute a `camera_route` message carries, or None."""
    now = time.time() if now is None else now
    if not isinstance(data, dict):
        return None
    try:
        camera = data.get("camera")
        if not isinstance(camera, dict):
            return None
        stamp = _stamp(camera, now)
        machine = camera.get("machine")
        if stamp is None or (machine is not None and not _machine(machine)):
            return None
        entry = model.CameraEntry(machine, _ref(camera.get("device")) if machine is not None else None, stamp)
        quality = data.get("quality")
        if quality is None:
            size = model.QUALITY_DEFAULT
        else:
            if not isinstance(quality, dict) or quality.get("size") not in model.QUALITIES:
                return None
            stamp = _stamp(quality, now)
            if stamp is None:
                return None
            size = model.QualityEntry(quality["size"], stamp)
    except ValueError:
        return None
    extra = {name: value for name, value in data.items()
             if name not in ("camera", "quality") and isinstance(name, str) and _WORD.fullmatch(name)}
    if len(extra) > MAX_EXTRA:
        return None
    return model.CameraRoute(entry, size, extra)


def read_codec(value):
    if not isinstance(value, dict) or value.get("name") != "h264" or value.get("profile") not in PROFILES:
        return None
    w, h = value.get("w"), value.get("h")
    if not _int(w, 64, 1920) or not _int(h, 64, 1080) or w % 2 or h % 2:
        return None
    if value.get("fps") not in (15, 30) or not _int(value.get("bitrate"), 100_000, 20_000_000):
        return None
    if not _int(value.get("mtu"), 576, 1400) or not isinstance(value.get("ltr"), bool):
        return None
    return {name: value[name] for name in ("name", "profile", "w", "h", "fps", "bitrate", "mtu", "ltr")}


def read_open(data, now=None):
    now = time.time() if now is None else now
    if not isinstance(data, dict) or not _int(data.get("stream"), 1, MAX_STREAM):
        return None
    flow = data.get("flow")
    if not isinstance(flow, str) or not FLOW_SHAPE.fullmatch(flow) or not _key_b64(data.get("key")):
        return None
    codec_data = read_codec(data.get("codec"))
    stamp = _stamp(data.get("for"), now)
    if codec_data is None or stamp is None:
        return None
    by = data["for"].get("by")
    try:
        camera = _ref(data.get("camera"))
    except ValueError:
        return None
    return {"stream": data["stream"], "flow": flow, "codec": codec_data, "key": data["key"], "camera": camera,
            "for": stamp, "by": by}


def read_opened(data):
    if not isinstance(data, dict) or not _int(data.get("stream"), 1, MAX_STREAM) or not isinstance(data.get("ok"), bool):
        return None
    error = data.get("error")
    return {"stream": data["stream"], "ok": data["ok"],
            "error": None if data["ok"] else (error if error in OPEN_ERRORS else "unsupported")}


def read_close(data):
    if not isinstance(data, dict) or not _int(data.get("stream"), 1, MAX_STREAM):
        return None
    reason = data.get("reason")
    return {"stream": data["stream"], "reason": reason if reason in CLOSE_REASONS else "error"}


_READERS = {OPENED: read_opened, CLOSE: read_close}
_TIMED = {DEVICES: read_devices, ROUTE: read_route, OPEN: read_open}


def read(msg, now=None):
    """(type, validated data) of a `camera_*` message, or None when it is not one or is malformed."""
    if not is_camera(msg) or msg["type"] not in TYPES:
        return None
    data = msg.get("data")
    try:
        if msg["type"] in _TIMED:
            value = _TIMED[msg["type"]](data, now)
        else:
            value = _READERS[msg["type"]](data)
    except (TypeError, ValueError, KeyError, AttributeError):
        return None
    return None if value is None else (msg["type"], value)
