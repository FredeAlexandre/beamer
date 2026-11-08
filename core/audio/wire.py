# SPDX-License-Identifier: GPL-3.0-only
"""The `audio_*` messages on the Beamer link (dev/audio/PHASE2.md §5): builders and validators.

Pure. A message is the link's `{"type", "data"}` object. `read` returns the validated data of an
`audio_*` message, or None for anything malformed: a malformed audio message is ignored, it never
ends a link (§5.1). Unknown fields are ignored and dropped.
"""

import base64
import binascii
import json
import re
import time

from core import protocol, settings_sync
from core.audio import model

VERSION = 1
MIN_VERSION = 1

HELLO = "audio_hello"
DEVICES = "audio_devices"
ROUTE = "audio_route"
FLOW = "audio_flow"
OPEN = "audio_open"
OPENED = "audio_opened"
CLOSE = "audio_close"
FRAME = "audio_frame"
WATCH = "audio_watch"
STATS = "audio_stats"
TEST = "audio_test"
TYPES = (HELLO, DEVICES, ROUTE, FLOW, OPEN, OPENED, CLOSE, FRAME, WATCH, STATS, TEST)
FLOODED = (DEVICES, ROUTE)

MAX_SIZE = 16 * 1024
MAX_FLOODED_SIZE = 32 * 1024
MAX_DEVICES = 32
MAX_KEY = 200
MAX_NAME = 128
MAX_ROUTE_MACHINES = 33
MAX_FRAME_PACKETS = 4
MAX_PACKET_TEXT = 2048
MAX_STREAM = 2 ** 31 - 1
MAX_HOWLS = 8
KEY_SIZE = 32
HELPER_STATES = ("ok", "starting", "failed")
OPEN_ERRORS = ("no_device", "no_virtual_mic", "conflict", "helper", "unsupported")
CLOSE_REASONS = ("route", "demand", "device_gone", "helper", "shutdown", "error")
KINDS = ("mic", "system")
TRANSPORTS = ("builtin", "usb", "bluetooth", "virtual", "hdmi", "other")
FLOW_SHAPE = re.compile(r"[0-9a-f]{8}")
_WORD = re.compile(r"[A-Za-z0-9_.:-]{1,32}")

CODECS = {
    "mic": {"name": "opus", "app": "voip", "channels": 1, "bitrate": 32000, "frame_ms": 10, "fec": True},
    "system": {"name": "opus", "app": "audio", "channels": 2, "bitrate": 96000, "frame_ms": 10, "fec": False},
}


def message(kind, data):
    return {"type": kind, "data": data}


def size_ok(msg):
    """Whether a message fits its limit (§5.1) once encoded as the link encodes it."""
    limit = MAX_FLOODED_SIZE if msg.get("type") in FLOODED else MAX_SIZE
    return len(json.dumps(msg, separators=(",", ":")).encode("utf-8")) < limit


def is_audio(msg):
    return isinstance(msg, dict) and isinstance(msg.get("type"), str) and msg["type"].startswith("audio_")


# Builders ---------------------------------------------------------------------------------------

def hello(platform, helper, listen, features=()):
    return message(HELLO, {"v": VERSION, "min_v": MIN_VERSION, "platform": platform,
                           "helper": helper if helper in HELPER_STATES else "failed",
                           "udp": {"listen": bool(listen)}, "codecs": ["opus"], "features": sorted(features)})


def devices(machine):
    return message(DEVICES, model.machine_data(machine))


def route(value):
    return message(ROUTE, model.route_data(value))


def flow_offer(flow, port, key):
    return message(FLOW, {"flow": flow, "phase": "offer", "port": port, "key": key})


def flow_answer(flow, port):
    return message(FLOW, {"flow": flow, "phase": "answer", "port": port})


def flow_want():
    return message(FLOW, {"phase": "want"})


def flow_close(flow):
    return message(FLOW, {"flow": flow, "phase": "close"})


def open_stream(stream, flow, kind, codec, key, sink, stamp):
    return message(OPEN, {"stream": stream, "flow": flow, "kind": kind, "codec": dict(codec), "key": key,
                          "sink": sink, "for": model.stamp_data(stamp)})


def opened(stream, ok, error=None):
    return message(OPENED, {"stream": stream, "ok": bool(ok), "error": None if ok else error})


def close(stream, reason):
    return message(CLOSE, {"stream": stream, "reason": reason})


def frame(flow, packets):
    return message(FRAME, {"flow": flow, "p": list(packets)})


def watch(on):
    return message(WATCH, {"on": bool(on)})


def stats(t, levels, streams, howls=()):
    """`howls`: the sender's howl guard notices (§3.3 `howl`), `{"to": machine, "freq_hz", "active"}`
    each; the field is left out when there is none."""
    data = {"t": int(t), "levels": levels, "streams": streams}
    if howls:
        data["howls"] = [dict(howl) for howl in howls]
    return message(STATS, data)


def test(device, seconds=10):
    return message(TEST, {"device": device, "seconds": seconds})


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


def _device_key(value):
    return _text(value, MAX_KEY, 1)


def _stamp(data, now):
    set_at, by = data.get("set_at"), data.get("by", "")
    if not _int(set_at, 0, settings_sync.MAX_STAMP) or set_at > now + settings_sync.DAY:
        return None
    if by != "" and not _machine(by):
        return None
    return model.Stamp(set_at, by)


def _ref(value):
    """A DeviceRef, None for null; raises ValueError when malformed."""
    if value is None:
        return None
    if not isinstance(value, dict) or not _device_key(value.get("key")) or not _text(value.get("name"), MAX_NAME):
        raise ValueError("bad device ref")
    return model.DeviceRef(value["key"], value["name"])


def _words(value, limit=32):
    if not isinstance(value, list) or len(value) > limit:
        return None
    return frozenset(word for word in value if isinstance(word, str) and _WORD.fullmatch(word))


def read_hello(data):
    if not isinstance(data, dict) or not _int(data.get("v"), 1, 2 ** 31) or not _int(data.get("min_v"), 1, 2 ** 31):
        return None
    udp = data.get("udp")
    if not isinstance(udp, dict) or not isinstance(udp.get("listen"), bool):
        return None
    features = _words(data.get("features", []))
    codecs = _words(data.get("codecs", []))
    if features is None or codecs is None or not _text(data.get("platform"), 16, 1):
        return None
    helper = data.get("helper")
    return {"v": data["v"], "min_v": data["min_v"], "platform": data["platform"],
            "helper": helper if helper in HELPER_STATES else "failed", "listen": udp["listen"],
            "codecs": codecs, "features": features}


def _devices(entries, kind, default):
    if not isinstance(entries, list) or len(entries) > MAX_DEVICES:
        return None
    result, seen = [], set()
    for entry in entries:
        if not isinstance(entry, dict) or not _device_key(entry.get("key")) or not _text(entry.get("name"), MAX_NAME):
            return None
        if entry["key"] in seen:
            continue
        seen.add(entry["key"])
        channels = entry.get("channels", 0)
        transport = entry.get("transport")
        form = entry.get("form")
        result.append(model.Device(entry["key"], entry["name"], kind,
                                   transport if transport in TRANSPORTS else "other",
                                   channels if _int(channels, 0, 64) else 0,
                                   form if form in model.FORMS else "unknown", entry["key"] == default))
    return tuple(result)


def read_devices(data, now=None):
    """The MachineAudio an `audio_devices` message describes, or None."""
    now = time.time() if now is None else now
    if not isinstance(data, dict) or not _machine(data.get("machine")):
        return None
    if not _int(data.get("rev"), 0, settings_sync.MAX_STAMP) or data["rev"] > (now + settings_sync.DAY) * 1000:
        return None
    defaults = []
    for name in ("default_input", "default_output"):
        value = data.get(name)
        if value is not None and not _device_key(value):
            return None
        defaults.append(value)
    inputs = _devices(data.get("inputs", []), "input", defaults[0])
    outputs = _devices(data.get("outputs", []), "output", defaults[1])
    vmic = data.get("virtual_mic", {})
    features = _words(data.get("features", []))
    links = data.get("links", [])
    if (inputs is None or outputs is None or features is None or not isinstance(vmic, dict)
            or not isinstance(links, list) or len(links) > protocol.MAX_PEERS + 1
            or not all(_machine(link) for link in links)):
        return None
    kind = vmic.get("kind")
    if kind is not None and not _text(kind, 16, 1):
        return None
    status = data.get("status")
    return model.MachineAudio(
        id=data["machine"], name=data["name"] if _text(data.get("name"), MAX_NAME) else "",
        platform=data["platform"] if _text(data.get("platform"), 16) else "", rev=data["rev"],
        status=status if status in model.STATUSES else "failed", inputs=inputs, outputs=outputs,
        default_input=defaults[0], default_output=defaults[1],
        virtual_mic=model.VirtualMic(kind, vmic.get("present") is True, vmic.get("in_use") is True),
        features=features, links=frozenset(links) - {data["machine"]},
        helper=data["helper"] if _text(data.get("helper"), 32) else "")


def read_route(data, now=None):
    """The Route an `audio_route` message carries, or None."""
    now = time.time() if now is None else now
    if not isinstance(data, dict):
        return None
    try:
        mic = data.get("mic")
        if not isinstance(mic, dict):
            return None
        stamp = _stamp(mic, now)
        machine = mic.get("machine")
        if stamp is None or (machine is not None and not _machine(machine)):
            return None
        mic_route = model.MicRoute(machine, _ref(mic.get("device")), stamp)
        outputs_data = data.get("outputs", {})
        if not isinstance(outputs_data, dict) or len(outputs_data) > MAX_ROUTE_MACHINES:
            return None
        outputs = {}
        for owner, entry in outputs_data.items():
            if not _machine(owner) or not isinstance(entry, dict):
                return None
            stamp, to = _stamp(entry, now), entry.get("to")
            if stamp is None or (to is not None and (not _machine(to) or to == owner)):
                return None
            outputs[owner] = model.OutputRoute(to, _ref(entry.get("device")), stamp)
        forms_data = data.get("forms", {})
        if not isinstance(forms_data, dict) or len(forms_data) > 2 * model.MAX_FORMS:
            return None
        forms = {}
        for name, entry in forms_data.items():
            owner, _space, key = name.partition(" ") if isinstance(name, str) else ("", "", "")
            if not _machine(owner) or not _device_key(key) or not isinstance(entry, dict):
                return None
            stamp, form = _stamp(entry, now), entry.get("form")
            if stamp is None or (form is not None and form not in model.OVERRIDE_FORMS):
                return None
            forms[(owner, key)] = model.FormEntry(form, stamp)
    except ValueError:
        return None
    return model.Route(mic_route, outputs, model.trim_forms(forms))


def read_flow(data):
    if not isinstance(data, dict):
        return None
    phase = data.get("phase")
    if phase == "want":
        return {"phase": "want"}
    flow = data.get("flow")
    if phase not in ("offer", "answer", "close") or not isinstance(flow, str) or not FLOW_SHAPE.fullmatch(flow):
        return None
    if phase == "close":
        return {"phase": phase, "flow": flow}
    if not _int(data.get("port"), 0, 65535):
        return None
    if phase == "answer":
        return {"phase": phase, "flow": flow, "port": data["port"]}
    if not _key_b64(data.get("key")):
        return None
    return {"phase": phase, "flow": flow, "port": data["port"], "key": data["key"]}


def read_codec(codec, kind):
    if not isinstance(codec, dict) or codec.get("name") != "opus":
        return None
    if codec.get("app") not in ("voip", "audio", "lowdelay") or codec.get("channels") not in (1, 2):
        return None
    if not _int(codec.get("bitrate"), 6000, 510000) or codec.get("frame_ms") not in (5, 10, 20, 40, 60):
        return None
    if not isinstance(codec.get("fec"), bool):
        return None
    return {name: codec[name] for name in ("name", "app", "channels", "bitrate", "frame_ms", "fec")}


def read_open(data, now=None):
    now = time.time() if now is None else now
    if not isinstance(data, dict) or not _int(data.get("stream"), 1, MAX_STREAM):
        return None
    flow, kind = data.get("flow"), data.get("kind")
    if not isinstance(flow, str) or not FLOW_SHAPE.fullmatch(flow) or kind not in KINDS:
        return None
    codec = read_codec(data.get("codec"), kind)
    if codec is None or not _key_b64(data.get("key")):
        return None
    sink = data.get("sink")
    if not isinstance(sink, dict):
        return None
    try:
        if sink.get("virtual_mic") is True:
            sink = {"virtual_mic": True}
        elif "device" in sink:
            sink = {"device": _ref(sink["device"])}
        else:
            return None
    except ValueError:
        return None
    stamp = _stamp(data.get("for"), now) if isinstance(data.get("for"), dict) else None
    if stamp is None:
        return None
    return {"stream": data["stream"], "flow": flow, "kind": kind, "codec": codec, "key": data["key"],
            "sink": sink, "for": stamp}


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


def read_frame(data):
    if not isinstance(data, dict) or not isinstance(data.get("flow"), str) or not FLOW_SHAPE.fullmatch(data["flow"]):
        return None
    packets = data.get("p")
    if (not isinstance(packets, list) or not 1 <= len(packets) <= MAX_FRAME_PACKETS
            or not all(_text(packet, MAX_PACKET_TEXT, 1) for packet in packets)):
        return None
    return {"flow": data["flow"], "p": list(packets)}


def read_watch(data):
    if not isinstance(data, dict) or not isinstance(data.get("on"), bool):
        return None
    return {"on": data["on"]}


def _level(value):
    if (not isinstance(value, list) or len(value) != 2
            or not all(isinstance(number, (int, float)) and not isinstance(number, bool) and -200 <= number <= 50
                       for number in value)):
        return None
    return (float(value[0]), float(value[1]))


def _number(value):
    return float(value) if isinstance(value, (int, float)) and not isinstance(value, bool) else None


def read_stats(data):
    if not isinstance(data, dict) or not _int(data.get("t"), 0, 2 ** 53):
        return None
    levels_data, streams_data = data.get("levels", {}), data.get("streams", {})
    if (not isinstance(levels_data, dict) or len(levels_data) > 2 * MAX_DEVICES + 8
            or not isinstance(streams_data, dict) or len(streams_data) > 32):
        return None
    levels = {}
    for name, value in levels_data.items():
        side, _colon, key = name.partition(":") if isinstance(name, str) else ("", "", "")
        level = _level(value)
        if side in ("in", "out") and _device_key(key) and level is not None:
            levels[(side, key)] = level
    streams = {}
    for name, value in streams_data.items():
        if isinstance(name, str) and name.isdigit() and isinstance(value, dict):
            path = value.get("path")
            streams[int(name)] = {"path": path if path in ("udp", "link") else "",
                                  "jb_ms": _number(value.get("jb_ms")), "loss_pct": _number(value.get("loss_pct")),
                                  "rtt_ms": _number(value.get("rtt_ms"))}
    howls = []
    howls_data = data.get("howls", [])
    for value in howls_data[:MAX_HOWLS] if isinstance(howls_data, list) else ():
        if (isinstance(value, dict) and _machine(value.get("to")) and isinstance(value.get("active"), bool)
                and _number(value.get("freq_hz")) is not None and 0 <= value["freq_hz"] <= 24000):
            howls.append({"to": value["to"], "freq_hz": float(value["freq_hz"]), "active": value["active"]})
    return {"t": data["t"], "levels": levels, "streams": streams, "howls": howls}


def read_test(data):
    if not isinstance(data, dict) or not _device_key(data.get("device")):
        return None
    seconds = data.get("seconds", 10)
    return {"device": data["device"], "seconds": seconds if _int(seconds, 1, 15) else 10}


_READERS = {HELLO: read_hello, FLOW: read_flow, OPENED: read_opened, CLOSE: read_close, FRAME: read_frame,
            WATCH: read_watch, STATS: read_stats, TEST: read_test}
_TIMED = {DEVICES: read_devices, ROUTE: read_route, OPEN: read_open}


def read(msg, now=None):
    """(type, validated data) of an `audio_*` message, or None when it is not one or is malformed."""
    if not is_audio(msg) or msg["type"] not in TYPES:
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
