# SPDX-License-Identifier: GPL-3.0-only
"""Pure analysis for the camera E2E cases (PHASE2 §10.5): no ssh, no files of the live apps.

Everything is put on ONE time base, the Mac's monotonic clock in seconds (`mac` below). It is the clock of:
  - the Mac app's `mono` stamps in audio-dev-events.jsonl (Python time.monotonic() = mach_absolute_time),
  - the Mac helper's now_ns() (mach_absolute_time in ns), hence the capture time the synthetic source writes
    into the C1 code strip (ts100us = capture_ns / 1e5, modulo 2^32: wraps every 4.97 days, unwrapped here).
The PC has two clocks: its monotonic clock (QueryPerformanceCounter: the PC app's `mono`, pcside/pcagent
stamps) and its wall clock (camprobe's `t` = unix µs). `Clock` relates Mac mono <-> PC mono from the
clock.py ping link (dev/audio/e2e/clock.py, unchanged), `Linear` relates PC unix <-> PC mono from the pairs
pcside_cam.py writes every second.

Inputs are plain lists/dicts so the same code runs on C4's recordings (`c4_frames`, dry run on the Mac).
"""
from __future__ import annotations

import json
import math
from collections import Counter
from pathlib import Path

import numpy as np

FRAME_MS = 1000.0 / 30
TS_WRAP = 1 << 32          # code strip time, units of 100 µs
PLACEHOLDER_CLASSES = ("builtin", "slot3", "other", "placeholder", "pattern")


# ---------------------------------------------------------------- generic helpers

def pct(values, ps=(50, 95, 99)):
    a = np.asarray([v for v in values if v is not None and not (isinstance(v, float) and math.isnan(v))], float)
    if not len(a):
        return {"n": 0}
    out = {f"p{p}": round(float(np.percentile(a, p)), 2) for p in ps}
    out.update(n=int(len(a)), mean=round(float(a.mean()), 2), max=round(float(a.max()), 2),
               min=round(float(a.min()), 2))
    return out


def jl(path):
    """JSON lines of a file (missing file -> [])."""
    out = []
    p = Path(path)
    if not p.exists():
        return out
    for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
    return out


def check(name, value, ok, mark, note=None):
    """One pass/fail line of a result. ok=None means 'not measurable here' (counts as not passed)."""
    # bool(): a numpy bool is neither True nor False for `is` (verdict) and not JSON
    row = {"name": name, "value": value, "mark": mark, "pass": None if ok is None else bool(ok)}
    if note:
        row["note"] = note
    return row


def verdict(checks):
    if any(c["pass"] is False for c in checks):
        return "fail"
    if any(c["pass"] is None for c in checks):
        return "incomplete"
    return "pass"


# ---------------------------------------------------------------- clocks

class Linear:
    """y = a + b (x - x0), fitted by least squares; identity-slope fallback with < 3 points."""

    def __init__(self, xs, ys):
        xs, ys = np.asarray(xs, float), np.asarray(ys, float)
        if len(xs) == 0:
            raise ValueError("no clock samples")
        self.x0 = float(xs[0])
        if len(xs) >= 3 and float(xs.max() - xs.min()) > 1.0:
            self.b, self.a = (float(v) for v in np.polyfit(xs - self.x0, ys, 1))
        else:
            self.b, self.a = 1.0, float(np.median(ys - (xs - self.x0)))
        res = ys - (self.a + self.b * (xs - self.x0))
        self.resid_max = float(np.abs(res).max()) if len(res) else 0.0

    def __call__(self, x):
        return self.a + self.b * (x - self.x0)

    def inverse(self, y):
        return (y - self.a) / self.b + self.x0


class Clock:
    """Mac mono <-> PC mono from clock.py pings [(t_mac, rtt_s, off_s)], off = PC - Mac.

    Per 10 s window the lowest-RTT ping is kept (as C4's analyze.py), then off = a + b t is fitted; the error
    is at most RTT_min / 2 (path asymmetry). Pings with an RTT above 3x the window minimum never count."""

    def __init__(self, pings, window_s=10.0):
        if not pings:
            raise ValueError("no clock pings")
        p = np.asarray(pings, float)
        t, rtt, off = p[:, 0], p[:, 1], p[:, 2]
        win = ((t - t[0]) // window_s).astype(np.int64)
        keep = [int(np.where(win == w)[0][np.argmin(rtt[win == w])]) for w in np.unique(win)]
        self.fit = Linear(t[keep], off[keep])
        self.info = {"pings": int(len(p)), "windows": len(keep), "rtt_min_ms": round(float(rtt.min()) * 1e3, 2),
                     "rtt_p50_ms": round(float(np.median(rtt)) * 1e3, 2),
                     "rtt_p99_ms": round(float(np.percentile(rtt, 99)) * 1e3, 2),
                     "drift_ppm": round(self.fit.b * 1e6, 2), "resid_max_ms": round(self.fit.resid_max * 1e3, 3)}
        self.rtt_series = [(float(a), float(b)) for a, b in zip(t, rtt)]

    def mac_to_pc(self, t_mac):
        return t_mac + self.fit(t_mac)

    def pc_to_mac(self, t_pc):
        # solve t_mac + off(t_mac) = t_pc; off changes by ppm, so three fixed-point steps are exact to < 1 ns
        t = t_pc - self.fit.a
        for _ in range(3):
            t = t_pc - self.fit(t)
        return t

    def rtt_spikes(self, thresh_s=0.15):
        """Mac times where the ssh ping RTT exceeded `thresh_s` (Wi-Fi stalls, as logged by the harness)."""
        return [t for t, r in self.rtt_series if r > thresh_s]


class PcTime:
    """PC wall clock (unix µs, camprobe) -> Mac mono seconds, via PC mono."""

    def __init__(self, clock: Clock, pairs):
        # pairs: [{"mono": s, "unix": s}] written by pcside_cam.py on the PC
        self.clock = clock
        self.unix_to_mono = Linear([p["unix"] for p in pairs], [p["mono"] for p in pairs])

    def unix_us(self, t_us):
        return self.clock.pc_to_mac(self.unix_to_mono(t_us / 1e6))

    def pc_mono(self, t):
        return self.clock.pc_to_mac(t)


# ---------------------------------------------------------------- frames seen by the PC app

def parse_dump(text):
    """camprobe `--dump` lines "t_us class frame ts100us" -> [(t_us, cls, frame, ts)]."""
    rows = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 4:
            try:
                rows.append((int(parts[0]), parts[1], int(parts[2]), int(parts[3])))
            except ValueError:
                pass
    return rows


def parse_probe_out(text):
    """camprobe stdout: JSON lines; the summary is the last line with `ok`."""
    lines = []
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                lines.append(json.loads(line))
            except ValueError:
                pass
    summary = next((x for x in reversed(lines) if "ok" in x), None)
    return summary, lines


def unwrap_ts(ts100us, ref_mac_s):
    """Code strip time (u32, 100 µs units of the Mac's mach clock) -> Mac mono seconds, nearest to ref."""
    ref = ref_mac_s * 1e4
    k = round((ref - ts100us) / TS_WRAP)
    return (ts100us + k * TS_WRAP) / 1e4


class Frames:
    """One app read: every frame the app got, on the Mac time base.

    rows: [(t_mac, cls, frame, cap_mac)], cap_mac = capture time from the code strip (None if not `c1`)."""

    def __init__(self, rows, tag=""):
        self.tag = tag
        self.rows = rows

    @classmethod
    def from_dump(cls, dump_rows, to_mac, tag=""):
        out = []
        for t_us, c, f, ts in dump_rows:
            t = to_mac(t_us)
            out.append((t, c, f, unwrap_ts(ts, t) if c == "c1" else None))
        return cls(out, tag)

    def c1(self):
        return [r for r in self.rows if r[1] == "c1"]

    def first_c1(self, after=None):
        return next((r for r in self.rows if r[1] == "c1" and (after is None or r[0] >= after)), None)

    def new_frames(self):
        """First delivery of each Mac frame, in delivery order: [(t_mac, frame, cap_mac)]."""
        seen, out = set(), []
        for t, c, f, cap in self.rows:
            if c == "c1" and f not in seen:
                seen.add(f)
                out.append((t, f, cap))
        return out


def latency_ms(frames: Frames, skip_s=3.0, exclude=None):
    """capture -> app per new frame, skipping the first `skip_s` after the first picture (start, IDR).
    exclude(t_mac) -> ms to subtract (e.g. the lip-sync hold `sync_ms`), or None."""
    nf = frames.new_frames()
    if not nf:
        return []
    t_first = nf[0][0]
    out = []
    for t, f, cap in nf:
        if t - t_first < skip_s:
            continue
        v = (t - cap) * 1e3
        if exclude is not None:
            v -= exclude(t) or 0.0
        out.append((t, v))
    return out


def gaps(frames: Frames, start=None, end=None):
    """[(t_start, gap_ms)] between consecutive NEW Mac frames reaching the app (a repeat = frozen)."""
    nf = [r for r in frames.new_frames() if (start is None or r[0] >= start) and (end is None or r[0] <= end)]
    return [(nf[i][0], (nf[i + 1][0] - nf[i][0]) * 1e3) for i in range(len(nf) - 1)]


def frozen_episodes(frames: Frames, thresh_ms=100.0, start=None, end=None):
    """C4's definition: a gap > thresh between new frames; frozen time = gap - one frame period."""
    return [(t, g, g - FRAME_MS) for t, g in gaps(frames, start, end) if g > thresh_ms]


def frozen_in(frames: Frames, t0, t1, min_gap_ms=50.0):
    """(max frozen ms, total frozen ms) for gaps that END in [t0, t1]: one injection's damage."""
    nf = frames.new_frames()
    worst, total = 0.0, 0.0
    for i in range(len(nf) - 1):
        a, b = nf[i][0], nf[i + 1][0]
        if b < t0 or b > t1:
            continue
        g = (b - a) * 1e3
        if g > min_gap_ms:
            worst = max(worst, g - FRAME_MS)
            total += g - FRAME_MS
    return round(worst, 1), round(total, 1)


def integrity(frames: Frames, allow_placeholder_windows=()):
    """Code-strip checks after the first picture of the read (§10.5 "0 corrupt pictures"):
      out_of_order: a Mac frame older than one already shown,
      strip_time:   capture time not increasing with the frame number (a valid-looking but wrong strip),
      non_live:     frames that are not the Mac picture after the first one (unreadable strip = `other`
                    = a corrupt or placeholder picture), outside the allowed windows [(t0, t1)],
      black:        `uniform` pictures anywhere (a black/flat frame).
    Limit: a picture corrupt below the strip with an intact strip is not caught here (C4 had a PSNR
    oracle; the product has none); the engine's own `lost`/`skipped` counters are reported beside."""
    first = frames.first_c1()
    res = Counter()
    res["black"] = sum(1 for r in frames.rows if r[1] == "uniform")
    if first is None:
        return dict(res, frames=len(frames.rows), live=0)
    last_f, last_cap = -1, None
    allowed = lambda t: any(a <= t <= b for a, b in allow_placeholder_windows)  # noqa: E731
    kinds = Counter()
    for t, c, f, cap in frames.rows:
        if t < first[0]:
            continue
        kinds[c] += 1
        if c != "c1":
            if not allowed(t):
                res["non_live"] += 1
            continue
        if f < last_f:
            res["out_of_order"] += 1
        elif f > last_f:
            if last_cap is not None and cap is not None and cap <= last_cap:
                res["strip_time"] += 1
            last_f, last_cap = f, cap
    out = {k: int(res.get(k, 0)) for k in ("out_of_order", "strip_time", "non_live", "black")}
    out.update(frames=len(frames.rows), after_first=dict(kinds), live=int(kinds.get("c1", 0)))
    return out


def corrupt_count(integ):
    return integ.get("out_of_order", 0) + integ.get("strip_time", 0) + integ.get("non_live", 0)


def start_of_read(frames: Frames, t_open):
    """On-demand start of one app read opened at t_open (Mac time).

    first_ms: first Mac picture after the open; before it: the classes the app got (placeholder expected,
    never black, never a Mac picture from an older stream), first frame number (the stream's IDR is its
    first numbered frame on a fresh stream; the number restarts per stream)."""
    first = frames.first_c1(after=t_open)
    before = Counter(r[1] for r in frames.rows if r[0] < (first[0] if first else float("inf")))
    return {"first_ms": round((first[0] - t_open) * 1e3, 1) if first else None,
            "first_frame": first[2] if first else None,
            "before": dict(before), "black_before": int(before.get("uniform", 0)),
            "placeholder_before": sum(before.get(c, 0) for c in PLACEHOLDER_CLASSES),
            "frames": len(frames.rows)}


def first_class_after(frames: Frames, t, cls):
    r = next((r for r in frames.rows if r[0] >= t and r[1] == cls), None)
    return r[0] if r else None


# ---------------------------------------------------------------- helper events (audio-dev-events.jsonl)

def events_in(events, t0, t1, to_mac=None):
    """Events whose `mono` (mapped by to_mac for the PC) lies in [t0, t1]; adds `t` = Mac time."""
    out = []
    for e in events:
        m = e.get("mono")
        if not isinstance(m, (int, float)):
            continue
        t = to_mac(m) if to_mac else m
        if t0 <= t <= t1:
            e = dict(e)
            e["t"] = t
            out.append(e)
    return out


def video_stats(events, direction):
    """[(t, stream, stats)] of the video streams in `stats` events, dir 'send' (Mac) or 'recv' (PC)."""
    out = []
    for e in events:
        if e.get("ev") != "stats":
            continue
        for sid, st in (e.get("streams") or {}).items():
            if isinstance(st, dict) and st.get("kind") == "video" and st.get("dir") == direction:
                out.append((e["t"], int(sid), st))
    return out


def audio_stats(events, direction):
    out = []
    for e in events:
        if e.get("ev") != "stats":
            continue
        for sid, st in (e.get("streams") or {}).items():
            if isinstance(st, dict) and st.get("kind") != "video" and st.get("dir") == direction:
                out.append((e["t"], int(sid), st))
    return out


def helper_cpu(events):
    """[(t, cpu_pct)] from the helper's own stats lines (emitted only while a stream runs)."""
    return [(e["t"], e["cpu_pct"]) for e in events if e.get("ev") == "stats" and isinstance(e.get("cpu_pct"), (int, float))]


def per_stream_last(stats_rows):
    """stream -> last stats dict (cumulative counters: idr, refresh, pli_rx, late, lost, holes ...)."""
    last = {}
    for t, sid, st in stats_rows:
        last[sid] = dict(st, t=t)
    return last


def counter_delta(stats_rows, key, t0, t1, stream=None):
    """Increase of a cumulative counter over [t0, t1] (per stream, summed; the first sample of a stream
    inside the window counts from 0 if the stream started inside it)."""
    by = {}
    for t, sid, st in stats_rows:
        if stream is not None and sid != stream:
            continue
        if key in st and isinstance(st[key], (int, float)):
            by.setdefault(sid, []).append((t, st[key]))
    total = 0
    for sid, series in by.items():
        before = [v for t, v in series if t < t0]
        inside = [v for t, v in series if t0 <= t <= t1]
        if not inside:
            continue
        base = before[-1] if before else 0
        total += max(inside) - base
    return total


def video_send_streams(events):
    """Mac video send streams: {stream: {"start": t, "running": t, "end": t, "error": code, "encoder": ...}}.
    A stream is video if a `camera` event or a video `stats` names it."""
    vids = {e["stream"] for e in events if e.get("ev") == "camera" and "stream" in e}
    vids |= {sid for _, sid, _ in video_stats(events, "send")}
    out = {}
    for e in events:
        sid = e.get("stream")
        if sid not in vids:
            continue
        s = out.setdefault(sid, {"stream": sid})
        if e.get("ev") == "stream_state":
            st = e.get("state")
            if st == "starting":
                s.setdefault("start", e["t"])
            elif st == "running":
                s.setdefault("running", e["t"])
            elif st in ("stopped", "failed"):
                s.setdefault("end", e["t"])
                if e.get("error"):
                    s["error"] = e["error"]
        elif e.get("ev") == "camera":
            st = e.get("state")
            s.setdefault("camera_" + str(st), e["t"])
            if e.get("encoder"):
                s["encoder"] = e["encoder"]
            if e.get("encoder_ms") is not None:
                s["encoder_ms"] = e.get("encoder_ms")
    for s in out.values():
        # camera on = from the first `camera`/`stream_state starting` to `camera stopped` (or stream end)
        on = [s[k] for k in ("start", "camera_starting", "camera_running") if k in s]
        s["on"] = min(on) if on else None
        off = [s[k] for k in ("camera_stopped",) if k in s] or ([s["end"]] if "end" in s else [])
        s["off"] = min(off) if off else None
    return out


def vcam_in_use_intervals(events, t_end):
    """PC: [(t_on, t_off)] while the helper's `vcam` event said in_use (an app had Beamer Camera open)."""
    out, on = [], None
    for e in events:
        if e.get("ev") != "vcam" or "in_use" not in e:
            continue
        if e["in_use"] and on is None:
            on = e["t"]
        elif not e["in_use"] and on is not None:
            out.append((on, e["t"]))
            on = None
    if on is not None:
        out.append((on, t_end))
    return out


def camera_on_invariant(send_streams, demand, t_end, start_hold=3.5, run_hold=5.5):
    """§6.5 checker: the Mac camera is never on without a receiver that wants it.

    demand: [(t_on, t_off)] while a PC app had Beamer Camera open (vcam in_use, or the harness's own reads).
    For every Mac video send stream, each stretch of its on-interval outside every demand interval must be
    at most `start_hold` (3.5 s: no PLI for 3 s + close) for a stream that never ran, `run_hold` (5.5 s: no
    feedback for 5 s + close; also covers the 3 s demand hold + stop after an app closes) for one that ran.
    Returns the violations [(stream, uncovered_s, t_from)]."""
    bad = []
    spans = sorted(demand)
    for sid, s in send_streams.items():
        if s.get("on") is None:
            continue
        a, b = s["on"], s.get("off") or t_end
        limit = run_hold if "running" in s else start_hold
        t = a
        for x, y in spans + [(b, b)]:
            if y <= t:
                continue
            seg_end = min(x, b)
            if seg_end - t > limit:
                bad.append((sid, round(seg_end - t, 2), round(t, 3)))
            t = max(t, y)
            if t >= b:
                break
    return bad


# ---------------------------------------------------------------- resources

def cpu_mem(samples, name, skip_s=60.0):
    """samples: [{"t": s, "procs": [{"name", "cpu" (s), "rss"|"priv" (bytes)}]}] -> CPU % of a core and
    memory first/last (MB) for the process `name`, ignoring the first `skip_s` (start-up)."""
    rows = []
    for smp in samples:
        for p in smp.get("procs", []):
            if p.get("name") == name and isinstance(p.get("cpu"), (int, float)):
                rows.append((smp["t"], p.get("pid"), float(p["cpu"]), p.get("rss") or p.get("priv") or p.get("ws")))
    if len(rows) < 2:
        return None
    t0 = rows[0][0]
    use = [r for r in rows if r[0] >= t0 + skip_s] or rows
    pid = use[-1][1]
    use = [r for r in use if r[1] == pid]
    if len(use) < 2:
        return None
    dt = use[-1][0] - use[0][0]
    mem = [r[3] for r in use if isinstance(r[3], (int, float))]
    return {"cpu_pct": round((use[-1][2] - use[0][2]) / dt * 100, 2) if dt > 0 else None,
            "mem_mb_first_last": [round(mem[0] / 2**20, 1), round(mem[-1] / 2**20, 1)] if mem else None,
            "mem_growth_mb": round((mem[-1] - mem[0]) / 2**20, 2) if mem else None,
            "minutes": round(dt / 60, 1), "pid": pid}


# ---------------------------------------------------------------- C4 recordings (dry run of the analysis)

def c4_load(raw_dir, name):
    """C4's raw logs (proto/e2e/out/raw/NAME.*) as Frames on the Mac clock (mach seconds), plus the Mac
    frame log and the cycle records. C4's clock: receiver pings (PC unix µs t1/t2, Mac mach ns)."""
    raw = Path(raw_dir)
    mac = jl(raw / f"{name}.mac.jsonl")
    rcv = jl(raw / f"{name}.recv.jsonl")
    pings = [e for e in rcv if e.get("ev") == "ping"]
    # PC unix (s) vs Mac mach (s): same lowest-RTT-per-window fit as Clock, on (t_mac, rtt, off) triples
    trip = [(p["mac"] / 1e9, (p["t2"] - p["t1"]) / 1e6, (p["t1"] + p["t2"]) / 2e6 - p["mac"] / 1e9) for p in pings]
    clock = Clock(trip, window_s=5.0)
    frames_raw, cycles, clocks = [], [], []
    import re
    p = raw / f"{name}.chrome.log"
    for line in p.read_text(errors="replace").splitlines() if p.exists() else []:
        m = re.search(r'"(C4[FCK]) (.*)", source:', line)
        if not m:
            continue
        tag, body = m.group(1), m.group(2).replace('\\"', '"')
        try:
            o = json.loads(body)
        except ValueError:
            continue
        if tag == "C4F":
            frames_raw += o
        elif tag == "C4C":
            cycles.append(o)
        else:
            clocks.append(o)
    # Chrome's performance clock drifts against Date (C4: -25 ppm): fit date - perf, correct (§10.5)
    ck = [(c["t"], c["date_minus_perf_ms"] * 1000) for c in clocks if "date_minus_perf_ms" in c and "t" in c]
    fix = (lambda t: t)
    if ck:
        lin = Linear([x for x, _ in ck], [y for _, y in ck])
        fix = (lambda t, lin=lin: t + lin(t))
    cls_of = {-1: "pattern", -2: "placeholder", -3: "slot3", -9: "other"}
    to_mac = lambda t_us: clock.pc_to_mac(fix(t_us) / 1e6)  # noqa: E731
    rows = []
    for k, ts, t in frames_raw:
        tm = to_mac(t)
        if k >= 0:
            rows.append((tm, "c1", k, unwrap_ts(ts, tm)))
        else:
            rows.append((tm, cls_of.get(k, "other"), 0, None))
    for c in cycles:
        for key in ("t_call", "t_res", "first_any", "first_mac", "t_close", "t_close_done"):
            if c.get(key):
                c[key + "_mac"] = to_mac(c[key])
    mf = {e["f"]: e for e in mac if e.get("ev") == "f"}
    return Frames(rows, name), cycles, mf, clock
