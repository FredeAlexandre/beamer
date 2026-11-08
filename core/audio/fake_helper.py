# SPDX-License-Identifier: GPL-3.0-only
"""A helper that moves no sound, for tests (dev/audio/PHASE2.md §3, §11.1).

`FakeEngine` answers the §3.2 commands with the §3.3 events a real helper would send, keeping flows
and streams as plain dicts. `FakeHelper` wraps it in-process with `HelperProcess`'s interface
(`start`, `send`, `stop`, `retry`, `state`) and adds test controls (devices, `in_use`, crash).
`python -m core.audio.fake_helper --control ...` runs it over stdio, for `HelperProcess` tests; flags
after the usual ones make it misbehave on purpose.
"""

import argparse
import copy
import json
import sys
import threading
import time

VERSION = "0.0.0-fake"
FEATURES = {
    "macos": ["system_capture", "local_mute_tap", "mic_demand", "virtual_mic", "aec"],
    "windows": ["system_capture", "local_mute_endpoint", "mic_demand", "virtual_mic", "aec"],
    "linux": [],
}


def default_devices(platform="macos"):
    if platform == "windows":
        return {"inputs": [{"key": "{mic-wave3}", "name": "Wave:3 Mic In", "transport": "usb", "channels": 1,
                            "form": "microphone"}],
                "outputs": [{"key": "{out-g733}", "name": "Logitech G733", "transport": "usb", "channels": 2,
                             "form": "headset"},
                            {"key": "{out-hdmi}", "name": "LG HDR 4K", "transport": "hdmi", "channels": 2,
                             "form": "speakers"}],
                "default_input": "{mic-wave3}", "default_output": "{out-g733}",
                "virtual_mic": {"kind": "vbcable", "present": True, "in_use": False}}
    return {"inputs": [{"key": "BuiltInMicrophoneDevice", "name": "MacBook Air Microphone", "transport": "builtin",
                        "channels": 1, "form": "microphone"}],
            "outputs": [{"key": "BuiltInSpeakerDevice", "name": "MacBook Air Speakers", "transport": "builtin",
                         "channels": 2, "form": "speakers"}],
            "default_input": "BuiltInMicrophoneDevice", "default_output": "BuiltInSpeakerDevice",
            "virtual_mic": {"kind": "beamer", "present": True, "in_use": False}}


class FakeEngine:
    """The helper's command handling without sound: `handle(command)` -> list of events."""

    def __init__(self, platform="macos", devices=None, features=None, restored=0):
        self.platform = platform
        self.devices = copy.deepcopy(devices or default_devices(platform))
        self.features = list(FEATURES.get(platform, []) if features is None else features)
        self.restored = restored
        self.vmic_exclusive = False
        self.flows = {}
        self.streams = {}
        self.link_in = 0
        self.meter = False
        self.commands = []
        self.next_port = 50000
        self.mic_error = None     # tests: an error code every mic `send` gets (e.g. "permission")
        self.system_error = None  # tests: the same for system sends

    def ready(self):
        features = list(self.features)
        if self.vmic_exclusive and self.platform == "windows" and "vmic_exclusive" not in features:
            features.append("vmic_exclusive")
        return [{"ev": "ready", "proto": 1, "version": VERSION, "platform": self.platform,
                 "features": features, "restored": self.restored}, self.devices_event()]

    def devices_event(self):
        return dict({"ev": "devices"}, **copy.deepcopy(self.devices))

    def _keys(self, side):
        return {device["key"] for device in self.devices[side]}

    def handle(self, command):
        self.commands.append(command)
        name = command.get("cmd")
        req = command.get("req")
        method = getattr(self, f"_cmd_{name}", None) if isinstance(name, str) else None
        if method is None:
            return [self._error(req, "unknown_cmd", f"unknown command {name!r}")]
        try:
            events, error = method(command)
        except (KeyError, TypeError, ValueError) as problem:
            return [self._error(req, "bad_args", str(problem))]
        if error is not None:
            return events + [self._error(req, error, error)]
        if req is not None and name != "ping":
            events = events + [{"ev": "ok", "req": req}]
        return events

    @staticmethod
    def _error(req, code, msg):
        event = {"ev": "error", "code": code, "msg": msg}
        if req is not None:
            event["req"] = req
        return event

    def _cmd_devices(self, command):
        return [self.devices_event()], None

    def _cmd_ping(self, command):
        return [{"ev": "pong", "req": command.get("req")}], None

    def _cmd_quit(self, command):
        return [], None

    def _cmd_flow_open(self, command):
        flow, role = command["flow"], command["role"]
        if role not in ("offer", "answer") or not isinstance(command["key"], str):
            return [], "bad_args"
        host = command.get("host")
        if host is not None and not isinstance(host, str):
            return [], "bad_args"
        port = command.get("port") or 0
        if port == 0:
            self.next_port += 1
            port = self.next_port
        self.flows[flow] = {"peer": command.get("peer"), "role": role, "port": port, "remote": command.get("remote"),
                            "host": host,
                            "state": "probing"}
        events = [{"ev": "flow_bound", "flow": flow, "port": port}]
        if command.get("remote"):
            self.flows[flow]["state"] = "up"
            events.append({"ev": "flow_state", "flow": flow, "state": "up", "rtt_ms": 1.0})
        return events, None

    def _cmd_flow_remote(self, command):
        flow = self.flows.get(command["flow"])
        if flow is None:
            return [], "unknown_flow"
        flow["remote"] = {"host": command["host"], "port": command["port"]}
        flow["state"] = "up"
        return [{"ev": "flow_state", "flow": command["flow"], "state": "up", "rtt_ms": 1.0}], None

    def _cmd_flow_close(self, command):
        if self.flows.pop(command["flow"], None) is None:
            return [], "unknown_flow"
        events = [{"ev": "flow_state", "flow": command["flow"], "state": "closed", "rtt_ms": None}]
        for stream in [stream for stream, value in self.streams.items() if value.get("flow") == command["flow"]]:
            del self.streams[stream]
            events.append({"ev": "stream_state", "stream": stream, "state": "stopped", "device": None})
        return events, None

    def _cmd_link_in(self, command):
        if command["flow"] not in self.flows:
            return [], "unknown_flow"
        self.link_in += len(command["p"])
        return [], None

    def _device_for(self, side, ref):
        """The key in use for `{"device": key|null}`, or None when it does not exist."""
        key = ref.get("device")
        default = self.devices["default_input" if side == "inputs" else "default_output"]
        if key is None:
            return default
        return key if key in self._keys(side) else None

    def _cmd_send(self, command):
        if command["flow"] not in self.flows:
            return [], "unknown_flow"
        if command["kind"] == "mic":
            if self.mic_error:
                return [], self.mic_error
            source = command["source"]
            device = "test" if "test" in source else self._device_for("inputs", source)
            if device is None:
                return [], "no_device"
        else:
            if "system_capture" not in self.features:
                return [], "unsupported"
            if self.system_error:
                return [], self.system_error
            device = self.devices["default_output"]
        self.streams[command["stream"]] = dict(command, role="send")
        return [{"ev": "stream_state", "stream": command["stream"], "state": "running", "device": device}], None

    def _cmd_recv(self, command):
        if command["flow"] not in self.flows:
            return [], "unknown_flow"
        sink = command["sink"]
        if sink.get("virtual_mic"):
            if not self.devices["virtual_mic"]["present"]:
                return [], "no_virtual_mic"
            device = "virtual_mic"
        else:
            device = self._device_for("outputs", sink)
            if device is None:
                return [], "no_device"
        self.streams[command["stream"]] = dict(command, role="recv")
        return [{"ev": "stream_state", "stream": command["stream"], "state": "running", "device": device}], None

    def _cmd_loop(self, command):
        device = self._device_for("inputs", command["source"])
        if device is None:
            return [], "no_device"
        if not self.devices["virtual_mic"]["present"]:
            return [], "no_virtual_mic"
        self.streams[command["stream"]] = dict(command, role="loop")
        return [{"ev": "stream_state", "stream": command["stream"], "state": "running", "device": device}], None

    def _cmd_stop(self, command):
        if self.streams.pop(command["stream"], None) is None:
            return [], "unknown_stream"
        return [{"ev": "stream_state", "stream": command["stream"], "state": "stopped", "device": None}], None

    def _cmd_meter(self, command):
        self.meter = bool(command["on"])
        return [], None

    def _cmd_test_input(self, command):
        if command["device"] not in self._keys("inputs"):
            return [], "no_device"
        return [], None

    def levels(self, t=0):
        """A `levels` event for every open device and stream (-20 dBFS RMS, -6 peak)."""
        inputs = {self.devices["default_input"]: [-20.0, -6.0]} if self.devices["default_input"] else {}
        outputs = {self.devices["default_output"]: [-20.0, -6.0]} if self.devices["default_output"] else {}
        return {"ev": "levels", "t": t, "in": inputs, "out": outputs,
                "streams": {str(stream): [-20.0, -6.0] for stream in self.streams}}

    def stats(self):
        return {"ev": "stats", "streams": {str(stream): {"path": "udp", "jb_ms": 23.0, "target_ms": 20.0, "late": 0,
                                                         "expand": 0, "drop": 0, "loss_pct": 0.1, "fec": 0,
                                                         "ppm": 3.0, "rtt_ms": 1.0, "kbps": 32.0}
                                           for stream in self.streams}}


class FakeHelper:
    """In-process helper: commands are answered at once through `deliver`, like a helper whose events
    come back on its reader thread. Same interface as HelperProcess, plus test controls."""

    def __init__(self, cmd=None, deliver=None, *, platform="macos", devices=None, features=None,
                 auto_ready=True, restored=0):
        self.cmd = cmd
        self._deliver = deliver
        self.engine = FakeEngine(platform, devices, features, restored)
        self.auto_ready = auto_ready
        self._state = "stopped"
        self.sent = []
        self.starts = 0
        self.mute_link_out = False

    @property
    def state(self):
        return self._state

    def bind(self, deliver):
        self._deliver = deliver

    def _emit(self, events):
        for event in events:
            self._deliver(event)

    def start(self):
        self.starts += 1
        # The Windows helper reports `vmic_exclusive` when started with --vmic-exclusive (WP12).
        self.engine.vmic_exclusive = bool(self.cmd) and "--vmic-exclusive" in self.cmd
        self._state = "starting"
        self._deliver({"ev": "_state", "state": "starting", "why": "start"})
        if self.auto_ready:
            self.ready()

    def ready(self):
        self._state = "ok"
        self._emit(self.engine.ready())

    def send(self, command):
        if self._state != "ok":
            return False
        self.sent.append(command)
        self._emit(self.engine.handle(command))
        return True

    def stop(self, timeout=2.0):
        self._state = "stopped"
        self.engine.flows.clear()
        self.engine.streams.clear()

    def retry(self):
        if self._state == "failed":
            self.start()

    # Test controls ------------------------------------------------------------------------------

    def commands(self, name=None):
        return [command for command in self.sent if name is None or command.get("cmd") == name]

    def set_devices(self, **changes):
        self.engine.devices.update(copy.deepcopy(changes))
        if self._state == "ok":
            self._emit([self.engine.devices_event()])

    def set_in_use(self, in_use):
        vmic = dict(self.engine.devices["virtual_mic"], in_use=bool(in_use))
        self.set_devices(virtual_mic=vmic)

    def crash(self, restart=True, restored=0):
        """The helper died: its flows and streams are gone; it starts again unless `restart` is False."""
        self.engine.flows.clear()
        self.engine.streams.clear()
        self.engine.restored = restored
        self._state = "starting"
        self._deliver({"ev": "_state", "state": "starting", "why": "exited"})
        if restart:
            self.ready()

    def fail(self):
        self._state = "failed"
        self._deliver({"ev": "_state", "state": "failed", "why": "exited"})

    def emit(self, event):
        self._deliver(event)


# stdio ------------------------------------------------------------------------------------------

def _parse(argv):
    parser = argparse.ArgumentParser(prog="fake_helper")
    parser.add_argument("--control", action="store_true")
    parser.add_argument("--parent-pid", type=int)
    parser.add_argument("--state-dir")
    parser.add_argument("--exclude-pid", type=int)
    parser.add_argument("--dev", action="store_true")
    parser.add_argument("--vmic-exclusive", action="store_true")
    parser.add_argument("--log-level", default="info")
    parser.add_argument("--platform", default="macos")
    parser.add_argument("--no-pong-after", type=int, default=-1, help="stop answering pings after N")
    parser.add_argument("--exit-after", type=float, default=-1.0, help="exit with code 3 after N seconds")
    parser.add_argument("--stderr-burst", type=int, default=0, help="write N log lines at start")
    parser.add_argument("--no-ready", action="store_true")
    return parser.parse_args(argv)


def main(argv=None):
    args = _parse(sys.argv[1:] if argv is None else argv)
    engine = FakeEngine(args.platform)
    engine.vmic_exclusive = args.vmic_exclusive
    out_lock = threading.Lock()

    def write(event):
        with out_lock:
            sys.stdout.write(json.dumps(event, separators=(",", ":")) + "\n")
            sys.stdout.flush()

    if not args.control:
        print(json.dumps({"version": VERSION}))
        return 0
    for index in range(args.stderr_burst):
        sys.stderr.write(f"info: burst line {index}\n")
    sys.stderr.flush()
    if args.exit_after >= 0:
        timer = threading.Timer(args.exit_after, lambda: __import__("os")._exit(3))
        timer.daemon = True
        timer.start()
    if not args.no_ready:
        for event in engine.ready():
            write(event)
    sys.stderr.write("info: fake helper ready\n")
    sys.stderr.flush()
    pings = 0
    for line in sys.stdin:
        try:
            command = json.loads(line)
        except ValueError:
            continue
        if not isinstance(command, dict):
            continue
        if command.get("cmd") == "quit":
            break
        if command.get("cmd") == "ping":
            pings += 1
            if 0 <= args.no_pong_after < pings:
                continue
        for event in engine.handle(command):
            write(event)
        if command.get("cmd") == "meter" and command.get("on"):
            write(engine.levels(int(time.monotonic() * 1000)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
