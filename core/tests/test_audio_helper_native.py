# SPDX-License-Identifier: GPL-3.0-only
"""HelperProcess against the real helper on the null backend (`beamer-audio-null`, WP2).

Skipped when the binary is not built. $BEAMER_AUDIO_NULL names it explicitly, else
`native/audio/build/{mac,win}/beamer-audio-null[.exe]` is used; $BEAMER_AUDIO_NULL_ARGS adds
arguments (space-separated) before `--control`. Every UDP socket is bound towards 127.0.0.1.
"""

import base64
import os
import queue
import secrets
import shutil
import sys
import tempfile
import time
import unittest
from pathlib import Path

from core.audio import helper as helper_module
from core.audio import wire
from core.audio.helper import HelperProcess


def null_helper():
    named = os.environ.get("BEAMER_AUDIO_NULL")
    if named:
        return Path(named) if Path(named).is_file() else None
    system = "win" if sys.platform.startswith("win") else "mac"
    name = "beamer-audio-null.exe" if system == "win" else "beamer-audio-null"
    for folder in (system, "tests", ""):
        candidate = helper_module.REPO / "native" / "audio" / "build" / folder / name
        if candidate.is_file():
            return candidate
    return None


BINARY = null_helper()


class Events:
    def __init__(self):
        self.queue = queue.Queue()
        self.seen = []

    def __call__(self, event):
        self.queue.put(event)

    def wait(self, test, timeout=10.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                event = self.queue.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            self.seen.append(event)
            if test(event):
                return event
        raise AssertionError(f"no matching event; seen {[(e.get('ev'), e.get('code')) for e in self.seen][-20:]}")


def is_ev(name, **fields):
    return lambda event: event.get("ev") == name and all(event.get(key) == value for key, value in fields.items())


def key():
    return base64.b64encode(secrets.token_bytes(32)).decode()


@unittest.skipIf(BINARY is None, "beamer-audio-null is not built (WP2); set BEAMER_AUDIO_NULL to run")
class NativeHelperTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.folder, True)

    def spawn(self):
        extra = os.environ.get("BEAMER_AUDIO_NULL_ARGS", "").split()
        events = Events()
        process = HelperProcess(helper_module.command_line([str(BINARY)] + extra, self.folder, dev=True), events,
                                backoff=(0.2,), fail_limit=2)
        self.addCleanup(process.stop)
        process.start()
        return process, events

    def test_ready_devices_and_control_answers(self):
        process, events = self.spawn()
        ready = events.wait(is_ev("ready"))
        self.assertEqual(ready["proto"], 1)
        self.assertIn(ready["platform"], ("macos", "windows", "linux"))
        self.assertIsInstance(ready["features"], list)
        devices = events.wait(is_ev("devices"))
        for name in ("inputs", "outputs", "default_input", "default_output", "virtual_mic"):
            self.assertIn(name, devices)
        process.send({"cmd": "devices", "req": 1})
        events.wait(is_ev("ok", req=1))
        process.send({"cmd": "no_such_command", "req": 2})
        self.assertEqual(events.wait(is_ev("error", req=2))["code"], "unknown_cmd")
        process.send({"cmd": "stop", "stream": 12345, "req": 3})
        self.assertEqual(events.wait(is_ev("error", req=3))["code"], "unknown_stream")
        process.send({"cmd": "meter", "on": True, "period_ms": 50, "req": 4})
        events.wait(is_ev("ok", req=4))
        proc = process._proc
        process.stop(timeout=3)
        self.assertEqual(proc.returncode, 0)

    def test_two_helpers_stream_over_loopback(self):
        a, a_events = self.spawn()
        b, b_events = self.spawn()
        a_events.wait(is_ev("ready"))
        b_events.wait(is_ev("ready"))
        flow, flow_key = secrets.token_hex(4), key()
        # The offer side is pointed at loopback at once (port 9 until the answer), so no socket of a
        # test helper ever binds towards the LAN.
        a.send({"cmd": "flow_open", "req": 1, "flow": flow, "peer": "b", "role": "offer", "port": 0,
                "remote": {"host": "127.0.0.1", "port": 9}, "key": flow_key, "rr_ms": 10})
        a_port = a_events.wait(is_ev("flow_bound", flow=flow))["port"]
        a_events.wait(is_ev("ok", req=1))
        b.send({"cmd": "flow_open", "req": 1, "flow": flow, "peer": "a", "role": "answer", "port": 0,
                "remote": {"host": "127.0.0.1", "port": a_port}, "key": flow_key, "rr_ms": 10})
        b_port = b_events.wait(is_ev("flow_bound", flow=flow))["port"]
        b_events.wait(is_ev("ok", req=1))
        a.send({"cmd": "flow_remote", "flow": flow, "host": "127.0.0.1", "port": b_port})
        a_events.wait(is_ev("flow_state", flow=flow, state="up"))
        b_events.wait(is_ev("flow_state", flow=flow, state="up"))
        stream, stream_key = 4242, key()
        b.send({"cmd": "recv", "req": 2, "stream": stream, "flow": flow, "kind": "mic", "codec": wire.CODECS["mic"],
                "key": stream_key, "sink": {"device": None}})
        b_events.wait(is_ev("ok", req=2))
        a.send({"cmd": "send", "req": 2, "stream": stream, "flow": flow, "kind": "mic", "source": {"device": None},
                "codec": wire.CODECS["mic"], "key": stream_key, "mute_local": False, "aec": None})
        a_events.wait(is_ev("ok", req=2))
        a_events.wait(is_ev("stream_state", stream=stream, state="running"))
        b_events.wait(is_ev("stream_state", stream=stream, state="running"))
        stats = b_events.wait(lambda event: event.get("ev") == "stats" and str(stream) in event.get("streams", {})
                              and event["streams"][str(stream)].get("path") == "udp", timeout=6)
        self.assertGreaterEqual(stats["streams"][str(stream)]["jb_ms"], 0)
        a.send({"cmd": "stop", "stream": stream, "req": 3})
        a_events.wait(is_ev("ok", req=3))
        a.send({"cmd": "flow_close", "flow": flow, "req": 4})
        a_events.wait(is_ev("ok", req=4))


if __name__ == "__main__":
    unittest.main()
