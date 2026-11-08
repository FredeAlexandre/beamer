# SPDX-License-Identifier: GPL-3.0-only
"""Analyse one E2E run directory written by run.py; prints a summary and writes RUN/result.json.

Reuses proto/scenarios/analyze.py (Run, latency, glitches: Phase 1's method, unchanged) and sig.py.
Long recordings (the soak) go through the same functions in 60 s pieces (detect, pilot_glitches),
so memory stays bounded; the numbers are the same up to piece edges.

What it adds: the dev test source's markers (from the helper stats `test_t0_ns`), the helpers' own
per-interval stats and state changes (audio-dev-events.jsonl), CPU and memory of Beamer and the
helper on both machines, the PC endpoints' mute timeline, on-demand start, outages around the
recovery actions, and the loop check.
"""
from __future__ import annotations

import importlib.util
import json
import os
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
SCEN = HERE.parent / "proto" / "scenarios"
sys.path.insert(0, str(SCEN))
_spec = importlib.util.spec_from_file_location("proto_analyze", SCEN / "analyze.py")
pa = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pa)
from sig import RATE, SIGS, Injector, detect as _detect, lockin, pilot_glitches as _pilot_glitches  # noqa: E402

PIECE = 60 * RATE


def detect(x, sig, thr=0.4, min_gap_s=0.3):
    if len(x) <= 2 * PIECE:
        return _detect(x, sig, thr, min_gap_s)
    out = []
    over = RATE
    for a in range(0, len(x), PIECE):
        part = _detect(x[a:a + PIECE + over], sig, thr, min_gap_s)
        for j, s, amp in part:
            j += a
            if out and j - out[-1][0] < min_gap_s * RATE:
                if s > out[-1][1]:
                    out[-1] = (j, s, amp)
                continue
            out.append((j, s, amp))
    return out


def pilot_glitches(x, f, markers=(), **kw):
    if len(x) <= 2 * PIECE:
        return _pilot_glitches(x, f, markers=markers, **kw)
    acc = {"windows": 0, "glitch_windows": 0, "glitch_events": 0, "glitch_episodes": 0, "glitch_at_s": []}
    markers = np.asarray(sorted(markers), dtype=np.int64)
    n = len(x)
    for a in range(0, n, PIECE):
        b = min(n, a + PIECE)
        first, last = a == 0, b == n
        seg = x[a:b]
        ms = markers[(markers >= a) & (markers < b)] - a
        g = _pilot_glitches(seg, f, markers=list(ms), skip_s=2.0 if (first or last) else 0.02)
        for k in ("windows", "glitch_windows", "glitch_events", "glitch_episodes"):
            acc[k] += g.get(k, 0) or 0
        acc["glitch_at_s"] += [round(a / RATE + t, 2) for t in g.get("glitch_at_s", [])]
    secs = acc["windows"] * 480 / RATE
    acc["seconds"] = round(secs, 1)
    acc["events_per_min"] = round(acc["glitch_events"] / max(1e-9, secs / 60), 2)
    acc["episodes_per_min"] = round(acc["glitch_episodes"] / max(1e-9, secs / 60), 2)
    acc["glitch_at_s"] = acc["glitch_at_s"][:40]
    return acc


pa.detect = detect
pa.pilot_glitches = pilot_glitches


def jl(path):
    return pa.jl(str(path))


class Ctx:
    def __init__(self, d: Path):
        self.d = d
        self.meta = json.load(open(d / "meta.json"))
        self.mac_ev = jl(d / "mac_events.jsonl")
        self.pc_ev = jl(d / "pc" / "pc_events.jsonl")
        self.notes = jl(d / "notes.jsonl")

    def make_testsrc(self, run):
        """testsrc.log (gen-style marker times on that machine's clock) for each machine whose helper
        ran the dev test source in this run."""
        out = {}
        for side, events, path in (("mac", self.mac_ev, self.d / "testsrc.log"),
                                   ("pc", self.pc_ev, self.d / "pc" / "testsrc.log")):
            lo, hi = self.window(side, run, pad=5)
            stats = [e for e in events if e.get("ev") == "stats" and lo <= e["mono"] <= hi and "now_ns" in e]
            if not stats:
                continue
            off = min(e["mono"] - e["now_ns"] / 1e9 for e in stats)
            segs = {}
            for e in stats:
                for sid, s in e.get("streams", {}).items():
                    if s.get("dir") == "send" and s.get("test_t0_ns"):
                        key = (sid, s["test_t0_ns"])
                        segs[key] = max(segs.get(key, 0), e["mono"])
            if not segs:
                continue
            sig = (self.meta.get("test_signal", {}).get(side) or ("A" if side == "mac" else "B")).split(":")[0]
            inj = Injector(sig)
            lines = []
            for (sid, t0ns), last in sorted(segs.items(), key=lambda kv: kv[0][1]):
                t0 = t0ns / 1e9 + off
                k = 0
                while True:
                    t = t0 + inj.marker_index(k) / RATE
                    if t > last + 0.5:
                        break
                    lines.append(json.dumps({"event": "marker", "t": t, "stream": sid, "k": k}))
                    k += 1
            path.write_text("\n".join(lines) + "\n")
            out[side] = {"segments": len(segs), "markers": len(lines), "helper_clock_offset_s": round(off, 6)}
        return out

    def window(self, side, run, pad=0.0):
        """(lo, hi) of this run on `side`'s monotonic clock."""
        lo, hi = self.meta["t0"] - pad, self.meta["t_end"] + pad
        if side == "pc":
            off = run.off
            lo, hi = lo + float(off(lo)), hi + float(off(hi))
        return lo, hi


def to_rel(run, ctx, side, t):
    """A time on `side`'s clock -> seconds since the run start on the Mac clock."""
    tm = float(run.to_mac("pc/x", t)) if side == "pc" else float(t)
    return round(tm - ctx.meta["t0"], 3)


def helper_stats(run, ctx, side):
    """Per-stream aggregates of the helper's per-interval stats while the route was set."""
    events = ctx.mac_ev if side == "mac" else ctx.pc_ev
    lo_m, hi_m = ctx.meta["t0"] + 2.0 + 3.0, ctx.meta.get("t_idle", ctx.meta["t_end"])
    if side == "pc":
        lo_m, hi_m = lo_m + float(run.off(lo_m)), hi_m + float(run.off(hi_m))
    stats = [e for e in events if e.get("ev") == "stats" and lo_m <= e["mono"] <= hi_m]
    cpu = [e.get("cpu_pct", 0) for e in stats]
    by = {}
    for e in stats:
        for sid, s in e.get("streams", {}).items():
            by.setdefault(sid, []).append(s)
    streams = {}
    for sid, rows in by.items():
        d0 = rows[0]
        r = {"dir": d0.get("dir"), "kind": d0.get("kind"), "intervals": len(rows),
             "paths": sorted({x.get("path") for x in rows})}
        if d0.get("dir") == "recv":
            for k in ("late", "expand", "drop", "flushed", "plc", "underrun", "fec", "dec_err"):
                r[k] = int(sum(x.get(k, 0) or 0 for x in rows))
            for k in ("jb_ms", "target_ms", "loss_pct", "rtt_ms", "kbps", "ppm"):
                v = [x[k] for x in rows if isinstance(x.get(k), (int, float))]
                r[k + "_mean"] = round(float(np.mean(v)), 2) if v else None
            v = [x["jb_max_ms"] for x in rows if isinstance(x.get("jb_max_ms"), (int, float))]
            r["jb_max_ms"] = max(v) if v else None
            r["run_jb_p99_ms_last"] = rows[-1].get("run_jb_p99_ms")
            p = [x["ppm"] for x in rows if isinstance(x.get("ppm"), (int, float))]
            if p:
                r["ppm_min"], r["ppm_max"] = min(p), max(p)
                r["ppm_last_60s_mean"] = round(float(np.mean(p[-60:])), 1)
            minutes = len(rows) / 60
            r["underrun_per_min"] = round(r["underrun"] / max(minutes, 1e-9), 2)
            r["expand_per_min"] = round(r["expand"] / max(minutes, 1e-9), 2)
        else:
            for k in ("kbps", "enc_us", "jb_ms", "rtt_ms"):
                v = [x[k] for x in rows if isinstance(x.get(k), (int, float))]
                r[k + "_mean"] = round(float(np.mean(v)), 2) if v else None
            r["overflow_last"] = rows[-1].get("overflow")
            r["enc_err_last"] = rows[-1].get("enc_err")
        streams[sid] = r
    return {"helper_cpu_pct_mean": round(float(np.mean(cpu)), 2) if cpu else None,
            "helper_cpu_pct_p95": round(float(np.quantile(cpu, 0.95)), 2) if cpu else None,
            "intervals": len(stats), "streams": streams}


def state_timeline(run, ctx, side, limit=80):
    events = ctx.mac_ev if side == "mac" else ctx.pc_ev
    lo, hi = ctx.window(side, run, pad=1)
    out = []
    for e in events:
        if not (lo <= e["mono"] <= hi):
            continue
        ev = e.get("ev")
        if ev in ("flow_state", "stream_state", "_state", "ready", "_link_up", "_link_down", "permission", "error"):
            item = {"t": to_rel(run, ctx, side, e["mono"]), "ev": ev}
            for k in ("state", "flow", "stream", "error", "peer", "mic", "system_audio", "code", "msg", "rtt_ms"):
                if k in e:
                    item[k] = e[k]
            out.append(item)
        elif ev == "devices":
            out.append({"t": to_rel(run, ctx, side, e["mono"]), "ev": "devices", "vmic_in_use": e.get("virtual_mic", {}).get("in_use"),
                        "outputs": len(e.get("outputs", [])), "inputs": len(e.get("inputs", []))})
    return out[:limit]


def resources(run, ctx):
    out = {}
    mac_rows = jl(ctx.d / "mac_resources.jsonl")
    pc_rows = jl(ctx.d / "pc" / "resources.jsonl")
    for side, rows, conv in (("mac", mac_rows, None), ("pc", pc_rows, "pc")):
        per = {}
        for r in rows:
            for p in r.get("procs", []):
                if "pid" not in p:
                    continue
                key = f"{p['name']}:{p['pid']}"
                mem = p.get("rss", p.get("priv"))
                per.setdefault(key, []).append((r["t"], p.get("cpu") or 0.0, mem, p.get("ws")))
        res = {}
        for key, v in per.items():
            if len(v) < 2:
                res[key] = {"samples": 1, "mem_mb": round((v[0][2] or 0) / 2**20, 1)}
                continue
            dt = v[-1][0] - v[0][0]
            res[key] = {"cpu_pct": round(100 * (v[-1][1] - v[0][1]) / dt, 2) if dt > 0 else None,
                        "mem_mb_first": round((v[0][2] or 0) / 2**20, 1), "mem_mb_last": round((v[-1][2] or 0) / 2**20, 1),
                        "mem_mb_max": round(max((x[2] or 0) for x in v) / 2**20, 1), "samples": len(v),
                        "span_s": round(dt, 1)}
            if v[0][3]:
                res[key]["ws_mb_first"], res[key]["ws_mb_last"] = round(v[0][3] / 2**20, 1), round(v[-1][3] / 2**20, 1)
        out[side] = res
    return out


def endpoint_timeline(run, ctx):
    rows = jl(ctx.d / "pc" / "endpoints.jsonl")
    out, last = [], {}
    for r in rows:
        for name, st in (r.get("endpoints") or {}).items():
            short = "G733" if "G733" in name else ("DualSense" if "DualSense" in name else name)
            val = (st.get("mute"), st.get("volume"))
            if last.get(short) != val:
                out.append({"t": to_rel(run, ctx, "pc", r["t"]), "endpoint": short, "mute": val[0], "volume": val[1]})
                last[short] = val
    return out


def presence(x, f, thr_db=-12.0):
    """Per 10 ms window: pilot present (lock-in amplitude within thr_db of the run's median)."""
    z = np.abs(lockin(x.astype(np.float64), f))
    if not len(z):
        return z.astype(bool)
    on = z[z > 1e-6]
    ref = np.median(on) if len(on) else 1e-6
    return 20 * np.log10(np.maximum(z, 1e-12) / ref) > thr_db


def outages(run, ctx, ref, sig, min_ms=30):
    """Pilot absences of at least min_ms at a sink, on the run's timeline (s since start)."""
    x, tm, _ = run.stream(ref)
    if x is None or tm is None:
        return None
    pr = presence(x, SIGS[sig]["pilot"])
    side = ref.split("/")[0]
    gaps = []
    on = np.nonzero(pr)[0]
    if not len(on):
        return {"present": False}
    first, lastw = on[0], on[-1]
    i = first
    while i <= lastw:
        if not pr[i]:
            j = i
            while j <= lastw and not pr[j]:
                j += 1
            if (j - i) * 10 >= min_ms:
                t_a = to_rel(run, ctx, side, float(tm(i * 480)))
                gaps.append({"from_s": t_a, "ms": (j - i) * 10})
            i = j
        else:
            i += 1
    return {"present": True, "first_s": to_rel(run, ctx, side, float(tm(first * 480))),
            "last_s": to_rel(run, ctx, side, float(tm(lastw * 480))), "gaps": gaps[:30], "n_gaps": len(gaps)}


def first_audio(run, ctx, ref):
    """When a sink recording first carries sound (> -60 dBFS over 10 ms), and when it started."""
    x, tm, log = run.stream(ref)
    if x is None or tm is None:
        return None
    n = len(x) // 480
    r = np.sqrt(np.mean(x[:n * 480].astype(np.float64).reshape(n, 480) ** 2, axis=1) + 1e-30)
    loud = np.nonzero(20 * np.log10(r) > -60)[0]
    side = ref.split("/")[0]
    start = to_rel(run, ctx, side, float(tm(0)))
    if not len(loud):
        return {"recorder_start_s": start, "first_audio_s": None}
    first = to_rel(run, ctx, side, float(tm(loud[0] * 480)))
    return {"recorder_start_s": start, "first_audio_s": first, "on_demand_ms": round((first - start) * 1000)}


def rms_per_s(run, ref):
    x, _, _ = run.stream(ref)
    if x is None:
        return None
    n = len(x) // RATE
    r = np.sqrt(np.mean(x[:n * RATE].astype(np.float64).reshape(n, RATE) ** 2, axis=1) + 1e-30)
    return [round(float(20 * np.log10(v)), 1) for v in r]


def band_per_s(run, ref, f):
    x, _, _ = run.stream(ref)
    if x is None:
        return None
    lo, hi = int(f) - 5, int(f) + 6
    return [round(float(20 * np.log10(np.mean(np.abs(np.fft.rfft(x[i:i + RATE] * np.hanning(RATE)))[lo:hi]) + 1e-12)), 1)
            for i in range(0, len(x) - RATE, RATE)]


def loop_check(run, ctx):
    out = {}
    for ref in sorted({r for p in ctx.meta["pairs"] for r in p[:2] if not r.endswith(("inj", "testsrc"))}):
        x, _, _ = run.stream(ref)
        if x is None:
            continue
        out[ref] = {}
        for sig in ("A", "B"):
            d = detect(x.astype(np.float64), sig, thr=0.5, min_gap_s=0.05)
            j = np.array([a for a, _, _ in d])
            close = int(np.sum(np.diff(j) < 0.6 * RATE)) if len(j) > 1 else 0
            out[ref][sig] = {"n": len(d), "pairs_closer_than_0.6s": close}
    return out


def main():
    d = Path(sys.argv[1])
    ctx = Ctx(d)
    # analyze.Run needs only meta.json and pings.json; the route window replaces Phase 1's transport run
    run = pa.Run(str(d))
    m = ctx.meta
    ts = ctx.make_testsrc(run)
    run.cache.clear()
    run.meta = dict(m, duration=m.get("t_idle", m["t_end"]) - m["t_send_start"] - 1.0)
    out = {"name": m["name"], "case": m["scenario"], "desc": m["desc"], "clock": run.clock, "testsrc": ts}
    out["latency"] = [pa.latency(run, *p) for p in m["pairs"]]
    gl = {}
    for src, sink, sig in m["pairs"]:
        for ref in (src, sink):
            if not ref.endswith(("inj", "testsrc")) and f"{ref}:{sig}" not in gl:
                gl[f"{ref}:{sig}"] = pa.glitches(run, ref, sig)
    out["glitches"] = gl
    out["helpers"] = {"mac": helper_stats(run, ctx, "mac"), "pc": helper_stats(run, ctx, "pc")}
    out["resources"] = resources(run, ctx)
    out["pc_endpoints"] = endpoint_timeline(run, ctx)
    out["timeline"] = {"mac": state_timeline(run, ctx, "mac"), "pc": state_timeline(run, ctx, "pc")}
    out["notes"] = [dict(n, t=round(n["t"] - m["t0"], 3)) for n in ctx.notes]
    spec = m.get("spec", {})
    if spec.get("demand"):
        out["on_demand"] = first_audio(run, ctx, spec["demand"])
    if spec.get("envelope"):
        a, b = spec["envelope"]
        out["rms_per_s"] = {a: rms_per_s(run, a), b: rms_per_s(run, b)}
    if spec.get("mute_check"):
        out["mac_mic_1025Hz_band_per_s"] = band_per_s(run, spec["mute_check"], 1025)
    if spec.get("loopcheck"):
        out["loop_check"] = loop_check(run, ctx)
    sinks = {(p[1], p[2]) for p in m["pairs"] if not p[1].endswith("inj")}
    if spec.get("actions") or spec.get("agg"):
        out["outages"] = {f"{r}:{s}": outages(run, ctx, r, s) for r, s in sorted(sinks)}
    json.dump(out, open(d / "result.json", "w"), indent=1, default=float)
    print(json.dumps(summary(out), indent=1, default=float))


def summary(out):
    s = {"case": out["case"], "clock_rtt_min_ms": out["clock"].get("rtt_min_ms")}
    s["latency"] = [{"pair": f"{x['src']}->{x['sink']}", "p50": x["p50_ms"], "p95": x["p95_ms"], "matched": x["matched"],
                     "n_src": x["n_src"], "missed": x["missed_at_s"][:5]} for x in out["latency"]]
    s["glitches"] = {k: (v or {}).get("events_per_min") if v else None for k, v in out["glitches"].items()}
    s["episodes"] = {k: (v or {}).get("episodes_per_min") if v else None for k, v in out["glitches"].items()}
    s["helper_cpu"] = {k: v["helper_cpu_pct_mean"] for k, v in out["helpers"].items()}
    s["recv"] = {side: {sid: {k: v.get(k) for k in ("kind", "paths", "jb_ms_mean", "target_ms_mean", "expand", "underrun",
                                                     "drop", "plc", "ppm_mean", "loss_pct_mean", "rtt_ms_mean")}
                        for sid, v in h["streams"].items() if v["dir"] == "recv"} for side, h in out["helpers"].items()}
    s["resources"] = out["resources"]
    for k in ("on_demand", "loop_check", "outages", "pc_endpoints"):
        if k in out:
            s[k] = out[k]
    return s


if __name__ == "__main__":
    main()
