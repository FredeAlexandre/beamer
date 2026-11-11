# SPDX-License-Identifier: GPL-3.0-only
"""Camera CW2 acceptance: two beamer-audio-null helpers stream a mic and the null camera over 127.0.0.1.

    python3 native/audio/tests/two_helpers_video.py --bin native/audio/build/tests/beamer-audio-null [--case all]
    (Windows: py -3 ...\\two_helpers_video.py --bin ...\\beamer-audio-null.exe)

Standard library only. The script plays Beamer's part (camera PHASE2 §3, §12 CW2): A sends its mic (sr:true) and
its null camera (720p30 2.5 Mbit/s unless stated) to B on one flow; B receives into its null virtual camera, whose
reference-checking fake decoder counts every kept picture that is not correct. link_out is relayed to link_in.
Cases (each with fresh helpers; --seconds is the length of the long runs, 120 by default):

  baseline   mic only (no video): audio late+plc rate and CPU for comparison
  clean      no loss: 0 frames missed after the start, first frame <= 300 ms after `send`, exactly 1 IDR
  slow       cam_start_ms=800: exactly 1 IDR, first frame <= 1.1 s after `send`
  1080       1080p 5 Mbit/s (coded 1088): 0 size rejections, 1 IDR, 0 frames missed
  noreceiver no `recv` on B: the sender ends `no_receiver` 3 s after `send`
  loss       1 % iid: frozen <= 200 ms/min, 0 IDRs after the start
  gilbert    0.5 % Gilbert (mean burst 6): frozen <= 400 ms/min
  stall      video_drop_ms 1500 (x3): clean again <= RTT + 2 frames after it ends, by refresh
  recovery   video_drop_au and a forced queue overflow: recovery by refresh
  link       drop_udp from the start: video <= 1.1 Mbit/s only in frame-tagged link_out events, feedback in ordinary
             ones, refreshes answer PLIs, the stream (opened in link state: provisional offset) plays within D
  sr         audio loss identical with sr:true and sr:false on a lossless run
Every video case: 0 incorrect kept pictures. Prints a JSON summary; exit 1 on any miss.
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

FLOW = "c0ffee01"
PORT = 24833  # not 24823-24830: the installed Beamer's helpers use those


def key(fill: int) -> str:
    return base64.b64encode(bytes([fill]) * 32).decode()


MIC_CODEC = {"name": "opus", "app": "voip", "channels": 1, "bitrate": 32000, "frame_ms": 10, "fec": True}


def vcodec(w=1280, h=720, br=2500000):
    return {"name": "h264", "profile": "constrained_high", "w": w, "h": h, "fps": 30, "bitrate": br, "mtu": 1200,
            "ltr": True}


class Helper:
    def __init__(self, name: str, binary: str, backend: str, video: str | None):
        self.name = name
        args = [binary, "--control", "--dev", "--parent-pid", str(os.getpid()), "--state-dir", ".",
                "--exclude-pid", str(os.getpid()), "--log-level", "warn", "--backend", "null:" + backend]
        args += ["--video", "null:" + video] if video is not None else ["--no-video"]
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        self.events: list[dict] = []
        self.cv = threading.Condition()
        self.wlock = threading.Lock()
        self.req = 0
        self.clock_off = None  # monotonic_ns - helper now_ns (min over stats events)
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=self._err, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            t = time.monotonic_ns()
            try:
                ev = json.loads(line)
            except ValueError:
                continue
            ev["_t"] = t
            if ev.get("ev") == "stats" and "now_ns" in ev:
                d = t - ev["now_ns"]
                self.clock_off = d if self.clock_off is None else min(self.clock_off, d)
            with self.cv:
                self.events.append(ev)
                self.cv.notify_all()

    def _err(self):
        for line in self.proc.stderr:
            s = line.decode(errors="replace")
            if "warn:" in s and "flow" not in s:
                sys.stderr.write(f"[{self.name}] {s}")

    def send(self, cmd: dict) -> int:
        with self.wlock:
            self.req += 1
            cmd = dict(cmd, req=self.req)
            self.proc.stdin.write((json.dumps(cmd) + "\n").encode())
            self.proc.stdin.flush()
            return self.req

    def wait(self, pred, timeout=5.0, start=0):
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

    def call(self, cmd: dict, timeout=5.0, ok_only=True) -> dict:
        n = len(self.events)
        r = self.send(cmd)
        _, ev = self.wait(lambda e: e.get("req") == r and e.get("ev") in ("ok", "error"), timeout, n)
        if ev is None or (ok_only and ev["ev"] == "error"):
            raise SystemExit(f"{self.name}: {cmd['cmd']} failed: {ev}")
        return ev

    def report(self) -> dict:
        n = len(self.events)
        self.call({"cmd": "dev", "null_video": {"report": True}})
        _, ev = self.wait(lambda e: e.get("ev") == "null_video", 3, n)
        return ev["report"] if ev else {}

    def stats(self, sid: str, since_ns: int = 0) -> list[dict]:
        with self.cv:
            return [dict(e["streams"][sid], _t=e["_t"]) for e in self.events
                    if e.get("ev") == "stats" and sid in e.get("streams", {}) and e["_t"] >= since_ns]

    def cpu(self, since_ns: int) -> float:
        with self.cv:
            v = [e.get("cpu_pct", 0) for e in self.events if e.get("ev") == "stats" and e["_t"] >= since_ns]
        return round(sum(v) / max(1, len(v)), 2)

    def to_mono(self, helper_ns: int) -> int:
        return helper_ns + (self.clock_off or 0)

    def quit(self):
        try:
            self.send({"cmd": "quit"})
            self.proc.stdin.close()
            self.proc.wait(5)
        except Exception:
            self.proc.kill()


class Relay:
    def __init__(self, a: Helper, b: Helper):
        self.a, self.b = a, b
        self.stop = threading.Event()
        self.video_events = self.feedback_events = self.bad_video = self.video_bytes = 0
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        idx = {self.a.name: 0, self.b.name: 0}
        while not self.stop.is_set():
            for src, dst in ((self.a, self.b), (self.b, self.a)):
                with src.cv:
                    evs = src.events[idx[src.name]:]
                idx[src.name] += len(evs)
                for e in evs:
                    if e.get("ev") != "link_out":
                        continue
                    if e.get("video"):
                        self.video_events += 1
                        self.video_bytes += sum(len(p) * 3 // 4 for p in e["p"])
                        if len(e["p"]) > 4 or "frame" not in e or "end" not in e:
                            self.bad_video += 1
                    else:
                        self.feedback_events += 1
                    dst.send({"cmd": "link_in", "flow": e["flow"], "p": e["p"]})
            time.sleep(0.002)


def open_pair(binary: str, a_video: str | None, b_video: str | None, link_only=False):
    A = Helper("A", binary, "mic=sine:440", a_video)
    B = Helper("B", binary, "", b_video)
    for h in (A, B):
        h.wait(lambda e: e.get("ev") == "ready", 5)
    relay = Relay(A, B)
    n = len(A.events)
    A.call({"cmd": "flow_open", "flow": FLOW, "peer": "B", "role": "offer", "port": 0, "host": "127.0.0.1",
            "remote": None, "key": key(9), "rr_ms": 10})
    _, fa = A.wait(lambda e: e.get("ev") == "flow_bound", 5, n)
    n = len(B.events)
    B.call({"cmd": "flow_open", "flow": FLOW, "peer": "A", "role": "answer", "port": PORT,
            "remote": {"host": "127.0.0.1", "port": fa["port"]}, "key": key(9), "rr_ms": 10})
    _, fb = B.wait(lambda e: e.get("ev") == "flow_bound", 5, n)
    if link_only:
        for h in (A, B):
            h.call({"cmd": "dev", "drop_udp": True})
    n = len(A.events)
    A.call({"cmd": "flow_remote", "flow": FLOW, "host": "127.0.0.1", "port": fb["port"]})
    want = "link" if link_only else "up"
    _, ev = A.wait(lambda e: e.get("ev") == "flow_state" and e.get("state") == want, 6, n)
    if ev is None:
        raise SystemExit(f"flow did not reach {want}")
    return A, B, relay


def start_mic(A: Helper, B: Helper, sr=True):
    A.call({"cmd": "send", "stream": 11, "flow": FLOW, "kind": "mic", "source": {"device": None}, "codec": MIC_CODEC,
            "key": key(1), "sr": sr})
    B.call({"cmd": "recv", "stream": 11, "flow": FLOW, "kind": "mic", "codec": MIC_CODEC, "key": key(1),
            "sink": {"virtual_mic": True}})


def start_video(A: Helper, B: Helper | None, codec=None, recv=True):
    codec = codec or vcodec()
    n = len(A.events)
    t_send = time.monotonic_ns()
    A.call({"cmd": "send", "stream": 21, "flow": FLOW, "kind": "video", "source": {"camera": None}, "codec": codec,
            "link": {"bitrate": 1000000, "fps": 15}, "key": key(2)})
    if recv and B:
        B.call({"cmd": "recv", "stream": 21, "flow": FLOW, "kind": "video", "codec": codec, "key": key(2),
                "sink": {"virtual_camera": True}})
    return t_send, n


def audio_summary(B: Helper, since: int, seconds: float) -> dict:
    rows = B.stats("11", since)
    late = sum(r.get("late", 0) for r in rows)
    plc = sum(r.get("plc", 0) for r in rows)
    return {"late_plc_per_min": round((late + plc) / max(seconds, 1) * 60, 2), "late": late, "plc": plc,
            "loss_pct_sum": round(sum(r.get("loss_pct", 0) for r in rows), 3), "dec_err": sum(r.get("dec_err", 0) for r in rows)}


def video_summary(A: Helper, B: Helper, since: int, seconds: float) -> dict:
    ra, rb = A.report(), B.report()
    rows = B.stats("21", since)
    last = rows[-1] if rows else {}
    frozen = sum(r.get("frozen_ms", 0) for r in rows)
    sa = A.stats("21", since)
    kbps = [r.get("kbps", 0) for r in sa]
    return {
        "enc": ra.get("enc", {}), "dec": rb.get("dec", {}), "sink": {k: rb.get("sink", {}).get(k) for k in
                                                                       ("presented", "incorrect_presented", "backwards", "w", "h")},
        "frozen_ms_per_min": round(frozen / max(seconds, 1) * 60, 1),
        "lost": last.get("lost"), "holes": last.get("holes"), "skipped": last.get("skipped"), "late": last.get("late"),
        "pli_tx": last.get("pli_tx"), "nack_tx": last.get("nack_tx"), "target_ms": last.get("target_ms"),
        "d_p99_ms": last.get("d_p99_ms"), "offset_ms": last.get("offset_ms"), "path": last.get("path"),
        "send_kbps_mean": round(sum(kbps) / max(1, len(kbps)), 1), "send_kbps_max": max(kbps, default=0),
        "send_path": sa[-1].get("path") if sa else None,
    }


def first_frame_ms(B: Helper, t_send: int) -> float | None:
    rb = B.report()
    fp = rb.get("sink", {}).get("first_present_ns", 0)
    return round((B.to_mono(fp) - t_send) / 1e6, 1) if fp else None


def no_incorrect(v: dict) -> bool:
    return v["dec"].get("incorrect_kept", 1) == 0 and v["sink"].get("incorrect_presented", 1) == 0 and \
        v["dec"].get("corrupt_input", 1) == 0 and v["enc"].get("number_mismatch", 1) == 0


def run_case(name: str, binary: str, seconds: float) -> dict:
    res: dict = {"case": name}
    ok = True
    if name == "baseline":
        A, B, relay = open_pair(binary, None, None)
        start_mic(A, B, sr=False)
        time.sleep(3)
        t0 = time.monotonic_ns()
        time.sleep(seconds)
        res["audio"] = audio_summary(B, t0, seconds)
        res["cpu_pct"] = {"A": A.cpu(t0), "B": B.cpu(t0)}
    elif name in ("clean", "slow", "1080"):
        cam = "cam_start_ms=800" if name == "slow" else "cam_start_ms=5"
        codec = vcodec(1920, 1080, 5000000) if name == "1080" else vcodec()
        A, B, relay = open_pair(binary, cam, "")
        start_mic(A, B)
        time.sleep(1)
        t_send, _ = start_video(A, B, codec)
        time.sleep(3)
        t0 = time.monotonic_ns()
        time.sleep(seconds)
        v = video_summary(A, B, t0, seconds)
        res["first_frame_ms"] = first_frame_ms(B, t_send)
        res["video"] = v
        res["audio"] = audio_summary(B, t0, seconds)
        res["cpu_pct"] = {"A": A.cpu(t0), "B": B.cpu(t0)}
        missed = v["enc"].get("frames", 0) - (v["sink"]["presented"] or 0)
        res["frames_missed"] = missed
        limit = 1100 if name == "slow" else 300
        ok &= res["first_frame_ms"] is not None and res["first_frame_ms"] <= limit
        ok &= v["enc"].get("idr") == 1 and no_incorrect(v)
        ok &= v["lost"] == 0 and v["skipped"] == 0 and missed <= 3  # in flight at the end
        if name == "1080":
            ok &= v["dec"].get("size_rejected") == 0 and v["sink"]["w"] == 1920 and v["sink"]["h"] == 1080
    elif name == "noreceiver":
        A, B, relay = open_pair(binary, "", "")
        n = len(A.events)
        start_video(A, B, recv=False)
        _, okev = A.wait(lambda e: e.get("ev") == "ok", 3, n)
        _, failed = A.wait(lambda e: e.get("ev") == "stream_state" and e.get("state") == "failed", 6, n)
        dt = None if not (okev and failed) else round((failed["_t"] - okev["_t"]) / 1e9, 3)
        res.update(failed=failed and failed.get("error"), after_s=dt, camera_open=A.report().get("camera", {}).get("open"))
        ok &= res["failed"] == "no_receiver" and dt is not None and abs(dt - 3.0) <= 0.05 and res["camera_open"] == 0
    elif name in ("loss", "gilbert"):
        A, B, relay = open_pair(binary, "", "")
        start_mic(A, B)
        time.sleep(1)
        start_video(A, B)
        time.sleep(3)
        A.call({"cmd": "dev", "video_loss": {"pct": 1, "burst": 0} if name == "loss" else {"pct": 0.5, "burst": 6}})
        t0 = time.monotonic_ns()
        time.sleep(seconds)
        v = video_summary(A, B, t0, seconds)
        res["video"] = v
        res["audio"] = audio_summary(B, t0, seconds)
        ok &= no_incorrect(v)
        if name == "loss":
            ok &= v["frozen_ms_per_min"] <= 200 and v["enc"].get("idr") == 1
        else:
            ok &= v["frozen_ms_per_min"] <= 400
    elif name == "stall":
        A, B, relay = open_pair(binary, "", "")
        start_mic(A, B)
        time.sleep(1)
        start_video(A, B)
        time.sleep(3)
        after = []
        for _ in range(3):
            refresh0 = A.report()["enc"].get("refresh", 0)
            A.call({"cmd": "dev", "video_drop_ms": 1500})
            time.sleep(1.5)
            B.call({"cmd": "dev", "null_video": {"mark": True}})
            time.sleep(1.5)
            rb, ra = B.report(), A.report()
            after.append({"kept_after_ms": rb["mark"]["kept_after_ms"], "refreshes": ra["enc"].get("refresh", 0) - refresh0,
                          "idr": ra["enc"].get("idr")})
            time.sleep(2)
        rtt = (A.stats("21") or [{}])[-1].get("rtt_ms") or 1.0
        res["drops"] = after
        res["limit_ms"] = round(rtt + 2 * 33.3 + 5, 1)  # RTT + 2 frames (+ the 5 ms null encoder latency)
        v = video_summary(A, B, 0, 22)
        ok &= no_incorrect(v) and v["enc"].get("idr") == 1
        ok &= all(d["kept_after_ms"] is not None and d["kept_after_ms"] <= res["limit_ms"] and d["refreshes"] >= 1 for d in after)
    elif name == "recovery":
        A, B, relay = open_pair(binary, "", "")
        start_mic(A, B)
        time.sleep(1)
        start_video(A, B)
        time.sleep(3)
        t_rec = time.monotonic_ns()
        r0 = A.report()["enc"].get("refresh", 0)
        for _ in range(3):
            A.call({"cmd": "dev", "video_drop_au": 1})
            time.sleep(2)
        A.call({"cmd": "dev", "video_stall_ms": 700})  # backpressure: input paused, nothing lost
        time.sleep(2)
        q0 = (A.stats("21") or [{}])[-1].get("q_drop")
        A.call({"cmd": "dev", "video_stall_nopause_ms": 900})  # hard cap: drops after numbering
        time.sleep(3)
        v = video_summary(A, B, t_rec, (time.monotonic_ns() - t_rec) / 1e9)
        sa = (A.stats("21") or [{}])[-1]
        res.update(video=v, q_drop_after_backpressure=q0, q_drop_after_hard_cap=sa.get("q_drop"),
                   refreshes=v["enc"].get("refresh", 0) - r0, shown=(B.stats("21") or [{}])[-1].get("shown"))
        ok &= no_incorrect(v) and v["enc"].get("idr") == 1 and res["refreshes"] >= 4
        ok &= q0 == 0 and (res["q_drop_after_hard_cap"] or 0) >= 1 and res["shown"] == "live"
    elif name == "link":
        A, B, relay = open_pair(binary, "", "", link_only=True)
        start_mic(A, B)
        time.sleep(1)
        t_send, _ = start_video(A, B)
        time.sleep(3)
        A.call({"cmd": "dev", "video_drop_au": 1})
        t0 = time.monotonic_ns()
        e0, f0, b0 = relay.video_events, relay.feedback_events, relay.video_bytes
        time.sleep(seconds)
        A.call({"cmd": "dev", "video_drop_au": 1})
        time.sleep(2)
        v = video_summary(A, B, t0, seconds)
        res.update(video=v, first_frame_ms=first_frame_ms(B, t_send), video_events=relay.video_events - e0,
                   feedback_events=relay.feedback_events - f0, bad_video_events=relay.bad_video,
                   video_kbps_on_link=round((relay.video_bytes - b0) * 8 / seconds / 1000, 1),
                   audio=audio_summary(B, t0, seconds), rr_rx=(A.stats("21") or [{}])[-1].get("rr_rx"))
        ok &= no_incorrect(v) and v["enc"].get("idr") == 1 and v["enc"].get("refresh", 0) >= 2
        ok &= res["video_kbps_on_link"] <= 1100 and v["send_path"] == "link" and res["bad_video_events"] == 0
        ok &= res["feedback_events"] > 0 and (res["rr_rx"] or 0) > 0
        # both helpers share one clock here, so the true offset is 0: the provisional seed must be close to it
        ok &= v["target_ms"] is not None and 50 <= v["target_ms"] <= 150 and v["offset_ms"] is not None
        ok &= abs(v["offset_ms"]) <= 25
        ok &= res["first_frame_ms"] is not None and res["first_frame_ms"] <= 2000
    elif name == "sr":
        out = {}
        for sr in (False, True):
            A, B, relay = open_pair(binary, "", "")
            start_mic(A, B, sr=sr)
            start_video(A, B)
            time.sleep(3)
            t0 = time.monotonic_ns()
            time.sleep(min(seconds, 30))
            out[f"sr_{str(sr).lower()}"] = audio_summary(B, t0, min(seconds, 30))
            relay.stop.set()
            A.quit()
            B.quit()
        res["audio"] = out
        ok &= out["sr_true"]["loss_pct_sum"] == out["sr_false"]["loss_pct_sum"] == 0
        res["pass"] = bool(ok)
        return res
    relay.stop.set()
    A.quit()
    B.quit()
    res["pass"] = bool(ok)
    return res


ALL = ["baseline", "clean", "slow", "1080", "noreceiver", "loss", "gilbert", "stall", "recovery", "link", "sr"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--case", default="all", help="comma-separated: " + ",".join(ALL))
    a = ap.parse_args()
    cases = ALL if a.case == "all" else a.case.split(",")
    results = []
    for c in cases:
        print(f"case {c}...", file=sys.stderr, flush=True)
        r = run_case(c, a.bin, a.seconds)
        print(json.dumps(r), file=sys.stderr, flush=True)
        results.append(r)
    by = {r["case"]: r for r in results}
    # cross-case marks: audio late+plc within +1/min of the run without video; engine video CPU
    if "baseline" in by and "clean" in by:
        base, clean = by["baseline"], by["clean"]
        d = clean["audio"]["late_plc_per_min"] - base["audio"]["late_plc_per_min"]
        cpu = {h: round(clean["cpu_pct"][h] - base["cpu_pct"][h], 2) for h in "AB"}
        by["clean"]["audio_vs_baseline_per_min"] = round(d, 2)
        by["clean"]["video_cpu_pct_vs_baseline"] = cpu
        by["clean"]["pass"] = by["clean"]["pass"] and d <= 1.0 and all(v < 1.0 for v in cpu.values())
    ok = all(r["pass"] for r in results)
    print(json.dumps({"pass": ok, "cases": by}, indent=1))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
