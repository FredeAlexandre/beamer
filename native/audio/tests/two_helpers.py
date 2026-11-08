# SPDX-License-Identifier: GPL-3.0-only
"""WP2 acceptance: two beamer-audio-null helpers stream mic + system sound both ways over 127.0.0.1.

    python3 native/audio/tests/two_helpers.py --bin native/audio/build/tests/beamer-audio-null
    (Windows: py -3 ...\\two_helpers.py --bin ...\\beamer-audio-null.exe)

Standard library only. This script plays Beamer's part: it drives both helpers over stdio, opens the
flow (A offers with the peer host and no bind, B answers on 24823), relays link_out -> link_in,
injects 2 % media loss on both sides (dev `loss_pct`) and runs A's null devices at +80 ppm. Then it
blocks UDP on A (dev `drop_udp`), times the fallback to `link`, restores UDP and times the return.
Pass marks (PHASE2 §13 WP2): JB level p99 < target + 15 ms, 0 decode errors, CPU < 3 % of a core per
helper, fallback <= 2.2 s, return <= 1.5 s. Prints a JSON summary; exit 1 on any miss.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import queue
import subprocess
import sys
import threading
import time


class Helper:
    def __init__(self, name: str, binary: str, backend: str):
        self.name = name
        self.proc = subprocess.Popen(
            [binary, "--control", "--dev", "--parent-pid", str(os.getpid()), "--state-dir", ".",
             "--exclude-pid", str(os.getpid()), "--log-level", "warn", "--backend", "null:" + backend],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        self.events: list[dict] = []
        self.cv = threading.Condition()
        self.wlock = threading.Lock()
        self.req = 0
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=self._err, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            try:
                ev = json.loads(line)
            except ValueError:
                print(f"{self.name}: bad line {line!r}", file=sys.stderr)
                continue
            ev["_t"] = time.monotonic()
            with self.cv:
                self.events.append(ev)
                self.cv.notify_all()

    def _err(self):
        for line in self.proc.stderr:
            sys.stderr.write(f"[{self.name}] {line.decode(errors='replace')}")

    def send(self, cmd: dict) -> int:
        with self.wlock:
            self.req += 1
            cmd = dict(cmd, req=self.req)
            self.proc.stdin.write((json.dumps(cmd) + "\n").encode())
            self.proc.stdin.flush()
            return self.req

    def wait(self, pred, timeout=5.0, start=0) -> tuple[int, dict | None]:
        end = time.monotonic() + timeout
        i = start
        with self.cv:
            while True:
                while i < len(self.events):
                    if pred(self.events[i]):
                        return i + 1, self.events[i]
                    i += 1
                left = end - time.monotonic()
                if left <= 0:
                    return i, None
                self.cv.wait(left)

    def call(self, cmd: dict, timeout=5.0) -> dict:
        n = len(self.events)
        r = self.send(cmd)
        _, ev = self.wait(lambda e: e.get("req") == r and e.get("ev") in ("ok", "error", "pong"), timeout, n)
        if ev is None or ev["ev"] == "error":
            raise SystemExit(f"{self.name}: {cmd['cmd']} failed: {ev}")
        return ev

    def quit(self):
        try:
            self.send({"cmd": "quit"})
            self.proc.stdin.close()
            self.proc.wait(5)
        except Exception:
            self.proc.kill()


def relay(a: Helper, b: Helper, stop: threading.Event, moved: list[int]):
    ia = ib = 0
    while not stop.is_set():
        for src, dst, attr in ((a, b, "ia"), (b, a, "ib")):
            i = ia if attr == "ia" else ib
            with src.cv:
                evs = src.events[i:]
            for e in evs:
                if e.get("ev") == "link_out":
                    dst.send({"cmd": "link_in", "flow": e["flow"], "p": e["p"]})
                    moved[0] += len(e["p"])
            if attr == "ia":
                ia += len(evs)
            else:
                ib += len(evs)
        time.sleep(0.005)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--loss", type=float, default=2.0)
    ap.add_argument("--ppm", type=float, default=80.0)
    ap.add_argument("--port", type=int, default=24823)
    ap.add_argument("--period", type=int, default=0, help="null device period in frames (0 = backend default)")
    ap.add_argument("--skip-fallback", action="store_true")
    a_ = ap.parse_args()

    per = f";period={a_.period}" if a_.period else ""
    A = Helper("A", a_.bin, f"ppm={a_.ppm:+g};mic=A;system=sine:1000:0.25" + per)
    B = Helper("B", a_.bin, "mic=B;system=sine:440:0.25" + per)
    print(f"pids A {A.proc.pid} B {B.proc.pid}", file=sys.stderr)
    for h in (A, B):
        if h.wait(lambda e: e.get("ev") == "ready", 5)[1] is None:
            raise SystemExit(f"{h.name}: no ready")
    flow = "a1b2c3d4"
    fkey = base64.b64encode(os.urandom(32)).decode()
    na = len(A.events)
    A.call({"cmd": "flow_open", "flow": flow, "peer": "B", "role": "offer", "port": 0, "host": "127.0.0.1",
            "remote": None, "key": fkey, "rr_ms": 10})
    pa = A.wait(lambda e: e.get("ev") == "flow_bound", 2, na)[1]["port"]
    nb = len(B.events)
    B.call({"cmd": "flow_open", "flow": flow, "peer": "A", "role": "answer", "port": a_.port,
            "remote": {"host": "127.0.0.1", "port": pa}, "key": fkey, "rr_ms": 10})
    pb = B.wait(lambda e: e.get("ev") == "flow_bound", 2, nb)[1]["port"]
    A.call({"cmd": "flow_remote", "flow": flow, "host": "127.0.0.1", "port": pb})
    for h in (A, B):
        if h.wait(lambda e: e.get("ev") == "flow_state" and e.get("state") == "up", 5)[1] is None:
            raise SystemExit(f"{h.name}: flow not up")
    stop = threading.Event()
    moved = [0]
    threading.Thread(target=relay, args=(A, B, stop, moved), daemon=True).start()

    mic = {"name": "opus", "app": "voip", "channels": 1, "bitrate": 32000, "frame_ms": 10, "fec": True}
    sysc = {"name": "opus", "app": "audio", "channels": 2, "bitrate": 96000, "frame_ms": 10, "fec": False}
    keys = {s: base64.b64encode(os.urandom(32)).decode() for s in (101, 102, 201, 202)}
    for snd, rcv, sid, kind in ((A, B, 101, "mic"), (A, B, 102, "system"), (B, A, 201, "mic"), (B, A, 202, "system")):
        codec = mic if kind == "mic" else sysc
        sink = {"virtual_mic": True} if kind == "mic" else {"device": None}
        rcv.call({"cmd": "recv", "stream": sid, "flow": flow, "kind": kind, "codec": codec, "key": keys[sid], "sink": sink})
        src = {"device": None} if kind == "mic" else {"system": True}
        snd.call({"cmd": "send", "stream": sid, "flow": flow, "kind": kind, "source": src, "codec": codec,
                  "key": keys[sid], "mute_local": False, "aec": None})
    for h in (A, B):
        h.call({"cmd": "dev", "loss_pct": a_.loss})
    t_start = time.monotonic()
    print(f"streaming {a_.seconds:.0f} s: 4 streams, {a_.loss}% loss, A at {a_.ppm:+g} ppm", file=sys.stderr)
    time.sleep(a_.seconds)
    t_end = time.monotonic()

    # ---- steady-state evaluation (skip the first 10 s)
    res: dict = {"seconds": a_.seconds, "loss_pct": a_.loss, "ppm": a_.ppm, "streams": {}, "cpu_pct": {}}
    ok = True
    for h, recv_ids in ((A, ("201", "202")), (B, ("101", "102"))):
        stats = [e for e in h.events if e.get("ev") == "stats" and t_start + 10 <= e["_t"] <= t_end]
        cpu = sorted(e.get("cpu_pct", 0) for e in stats)
        res["cpu_pct"][h.name] = {"mean": round(sum(cpu) / max(1, len(cpu)), 2),
                                  "p95": cpu[int(0.95 * (len(cpu) - 1))] if cpu else None}
        ok &= res["cpu_pct"][h.name]["mean"] < 3.0
        for sid in recv_ids:
            rows = [e["streams"][sid] for e in stats if sid in e["streams"]]
            over = sorted(r["jb_p99_ms"] - r["target_ms"] for r in rows)
            p99_over = over[int(0.99 * (len(over) - 1))] if over else None
            dec = sum(r["dec_err"] for r in rows)
            st = {
                "seconds": len(rows),
                "jb_mean_ms": round(sum(r["jb_ms"] for r in rows) / max(1, len(rows)), 1),
                "target_mean_ms": round(sum(r["target_ms"] for r in rows) / max(1, len(rows)), 1),
                "p99_level_minus_target_ms": None if p99_over is None else round(p99_over, 1),
                "max_level_ms": max((r["jb_max_ms"] for r in rows), default=None),
                "dec_err": dec,
                "loss_pct_mean": round(sum(r["loss_pct"] for r in rows) / max(1, len(rows)), 2),
                "fec": sum(r["fec"] for r in rows), "plc": sum(r["plc"] for r in rows),
                "expand": sum(r["expand"] for r in rows), "drop": sum(r["drop"] for r in rows),
                "ppm_i_last": rows[-1]["ppm_i"] if rows else None,
            }
            res["streams"][f"{h.name}:{sid}"] = st
            ok &= bool(rows) and p99_over is not None and p99_over < 15 and dec == 0

    # ---- link fallback and return (UDP blocked on A, both directions)
    if a_.skip_fallback:
        stop.set()
        for h in (A, B):
            h.quit()
        res["pass"] = bool(ok)
        print(json.dumps(res, indent=1))
        return 0 if ok else 1
    for h in (A, B):
        h.call({"cmd": "dev", "loss_pct": 0})
    marks = {}
    na, nb = len(A.events), len(B.events)
    t0 = time.monotonic()
    A.call({"cmd": "dev", "drop_udp": True})
    for h, n in ((A, na), (B, nb)):
        _, ev = h.wait(lambda e: e.get("ev") == "flow_state" and e.get("state") == "link", 5, n)
        marks[f"{h.name}_link_s"] = None if ev is None else round(ev["_t"] - t0, 3)
    time.sleep(5)
    via_link = [e["streams"] for e in B.events[nb:] if e.get("ev") == "stats"][-1]
    res["during_link"] = {"B:101": {k: via_link["101"][k] for k in ("path", "playing", "jb_ms", "rx_link")},
                          "relayed_packets": moved[0]}
    na, nb = len(A.events), len(B.events)
    t0 = time.monotonic()
    A.call({"cmd": "dev", "drop_udp": False})
    for h, n in ((A, na), (B, nb)):
        _, ev = h.wait(lambda e: e.get("ev") == "flow_state" and e.get("state") == "up", 5, n)
        marks[f"{h.name}_up_s"] = None if ev is None else round(ev["_t"] - t0, 3)
    res["fallback"] = marks
    ok &= all(marks.get(f"{n}_link_s") is not None and marks[f"{n}_link_s"] <= 2.2 for n in "AB")
    ok &= all(marks.get(f"{n}_up_s") is not None and marks[f"{n}_up_s"] <= 1.5 for n in "AB")
    ok &= res["during_link"]["B:101"]["path"] == "link" and res["during_link"]["B:101"]["playing"]
    stop.set()
    for h in (A, B):
        h.quit()
    res["pass"] = bool(ok)
    print(json.dumps(res, indent=1))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
