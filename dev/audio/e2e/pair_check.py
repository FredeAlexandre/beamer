# SPDX-License-Identifier: GPL-3.0-only
"""One machine, two helpers over 127.0.0.1: isolates one side of a Beamer audio path.

    python pair_check.py --send BIN[:null:<opts>] --recv BIN[:null:<opts>] --kind system|mic
                         [--source system|device:KEY|test:A] [--sink default|device:KEY|vmic]
                         [--seconds S] [--mute-local]

Each helper runs `--control --dev`; `BIN:null:<opts>` adds `--backend null:<opts>` (e.g.
`out=record:out.wav` records what the receiver renders, `system=A` is a test signal capture).
Prints the receiver's per-interval stats and the sender's, as JSON lines on stdout. Stdlib only.
Used by WP10 to tell a capture-side fault from a render-side fault without Beamer or the network.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import subprocess
import sys
import threading
import time


class Helper:
    def __init__(self, name, spec):
        binary, _, backend = spec.partition(":null:")
        cmd = [binary, "--control", "--dev", "--parent-pid", str(os.getpid()), "--state-dir", os.getcwd(),
               "--exclude-pid", str(os.getpid()), "--log-level", "info"]
        if backend:
            cmd += ["--backend", "null:" + backend]
        self.name = name
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        self.events, self.cv, self.req = [], threading.Condition(), 0
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=self._err, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            try:
                ev = json.loads(line)
            except ValueError:
                continue
            ev["_t"] = time.monotonic()
            with self.cv:
                self.events.append(ev)
                self.cv.notify_all()

    def _err(self):
        for line in self.p.stderr:
            sys.stderr.write(f"[{self.name}] {line.decode(errors='replace')}")

    def call(self, cmd, timeout=5.0):
        with self.cv:
            self.req += 1
            r = self.req
            n = len(self.events)
        self.p.stdin.write((json.dumps(dict(cmd, req=r)) + "\n").encode())
        self.p.stdin.flush()
        end = time.monotonic() + timeout
        with self.cv:
            while time.monotonic() < end:
                for e in self.events[n:]:
                    if e.get("req") == r and e.get("ev") in ("ok", "error"):
                        if e["ev"] == "error":
                            raise SystemExit(f"{self.name} {cmd['cmd']}: {e}")
                        return e
                self.cv.wait(0.05)
        raise SystemExit(f"{self.name} {cmd['cmd']}: no reply")

    def first(self, pred, timeout=5.0):
        end = time.monotonic() + timeout
        with self.cv:
            while time.monotonic() < end:
                for e in self.events:
                    if pred(e):
                        return e
                self.cv.wait(0.05)
        return None

    def quit(self):
        try:
            self.p.stdin.write(b'{"cmd":"quit"}\n')
            self.p.stdin.close()
            self.p.wait(5)
        except Exception:  # noqa: BLE001
            self.p.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--send", required=True)
    ap.add_argument("--recv", required=True)
    ap.add_argument("--kind", default="system")
    ap.add_argument("--source", default="system")
    ap.add_argument("--sink", default="default")
    ap.add_argument("--seconds", type=float, default=15)
    ap.add_argument("--mute-local", action="store_true")
    a = ap.parse_args()
    S, R = Helper("send", a.send), Helper("recv", a.recv)
    for h in (S, R):
        if not h.first(lambda e: e.get("ev") == "ready", 5):
            raise SystemExit(f"{h.name}: no ready")
    flow, fkey, skey = "e2e0c0de", base64.b64encode(os.urandom(32)).decode(), base64.b64encode(os.urandom(32)).decode()
    S.call({"cmd": "flow_open", "flow": flow, "peer": "R", "role": "offer", "port": 0, "host": "127.0.0.1",
            "remote": None, "key": fkey, "rr_ms": 10})
    port = S.first(lambda e: e.get("ev") == "flow_bound", 2)["port"]
    R.call({"cmd": "flow_open", "flow": flow, "peer": "S", "role": "answer", "port": 0,
            "remote": {"host": "127.0.0.1", "port": port}, "key": fkey, "rr_ms": 10})
    rport = R.first(lambda e: e.get("ev") == "flow_bound", 2)["port"]
    S.call({"cmd": "flow_remote", "flow": flow, "host": "127.0.0.1", "port": rport})
    codec = ({"name": "opus", "app": "audio", "channels": 2, "bitrate": 96000, "frame_ms": 10, "fec": False}
             if a.kind == "system" else
             {"name": "opus", "app": "voip", "channels": 1, "bitrate": 32000, "frame_ms": 10, "fec": True})
    sink = ({"virtual_mic": True} if a.sink == "vmic" else
            {"device": None if a.sink == "default" else a.sink.split(":", 1)[1]})
    R.call({"cmd": "recv", "stream": 77, "flow": flow, "kind": a.kind, "codec": codec, "key": skey, "sink": sink})
    if a.source == "system":
        source = {"system": True}
    elif a.source.startswith("test:"):
        source = {"test": a.source[5:]}
    else:
        source = {"device": a.source.split(":", 1)[1] if ":" in a.source else None}
    S.call({"cmd": "send", "stream": 77, "flow": flow, "kind": a.kind, "codec": codec, "key": skey, "source": source,
            "mute_local": a.mute_local, "aec": None})
    t0 = time.monotonic()
    print(json.dumps({"event": "start", "t": t0}), flush=True)
    time.sleep(a.seconds)
    for h in (S, R):
        for e in h.events:
            if e.get("ev") in ("stats", "stream_state", "flow_state", "error"):
                print(json.dumps(dict(e, who=h.name)), flush=True)
    S.call({"cmd": "stop", "stream": 77})
    R.call({"cmd": "stop", "stream": 77})
    S.quit()
    R.quit()


if __name__ == "__main__":
    main()
