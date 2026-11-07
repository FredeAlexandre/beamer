"""Analyse one scenario run directory written by run.py; prints and saves RUN/result.json.

One-way latency: a marker's time at the source (when the source capture delivered it; for a
`gen` injector, when it was written to the player) and at the sink (when the sink-side capture
delivered it), both from time.monotonic() on their machine, put on the Mac timeline with the
clock offset fitted to the transport's ping exchange (sig.clock_fit).
"""
from __future__ import annotations

import glob
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sig import RATE, SIGS, clock_fit, detect, pct, pilot_glitches, time_map  # noqa: E402


def jl(path):
    out = []
    if not os.path.exists(path):
        return out
    for line in open(path, errors="replace"):
        line = line.strip()
        if line.startswith("{"):
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
    return out


class Run:
    def __init__(self, d):
        self.d = d
        self.meta = json.load(open(os.path.join(d, "meta.json")))
        p = json.load(open(os.path.join(d, "pings.json")))["pings"]
        self.off, self.clock = clock_fit(p)
        self.cache = {}

    def path(self, ref):
        side, name = ref.split("/")
        return os.path.join(self.d, name) if side == "mac" else os.path.join(self.d, "pc", name)

    def stream(self, ref):
        if ref not in self.cache:
            p = self.path(ref)
            x = np.fromfile(p + ".f32", "<f4") if os.path.exists(p + ".f32") else None
            ch = np.fromfile(p + ".chunks", "<f8").reshape(-1, 2) if os.path.exists(p + ".chunks") else None
            self.cache[ref] = (x, time_map(ch) if ch is not None and len(ch) > 10 else None, jl(p + ".log"))
        return self.cache[ref]

    def to_mac(self, ref, t):
        t = np.asarray(t, float)
        if ref.startswith("mac/"):
            return t
        o = self.off(t - self.off(t))
        return t - o

    def markers(self, ref, sig, thr=0.4):
        """[(t_mac, similarity, amplitude)] of `sig` markers in a stream (or a gen log)."""
        x, tm, log = self.stream(ref)
        if x is None:  # gen injector: logged times
            ms = [e for e in log if e.get("event") == "marker"]
            return [(float(self.to_mac(ref, e["t"])), 1.0, None) for e in ms]
        mixed = [e["index"] for e in log if e.get("event") == "marker"]
        if mixed:  # injected into this stream by `tap.py src --mix`
            return [(float(self.to_mac(ref, tm(i))), 1.0, None) for i in mixed if i < len(x)]
        return [(float(self.to_mac(ref, tm(j))), s, a) for j, s, a in detect(x, sig, thr)]


def latency(run, src, sink, sig, window=0.6):
    """Pairs each source marker with the first unused sink detection 0..window s later. Source
    markers outside the sink recording's span (before it starts / after it stops) are ignored."""
    S = run.markers(src, sig)
    K = run.markers(sink, sig)
    kt = np.array([k[0] for k in K])
    used = np.zeros(len(K), bool)
    lat, sims, missed = [], [], []
    x, tm, _ = run.stream(sink)
    lo = hi = None
    if tm is not None:
        lo, hi = float(run.to_mac(sink, tm(0))), float(run.to_mac(sink, tm(len(x) - 1)))
    # and only while the transport ran (Mac send start + 1 s .. its end)
    t_on = run.meta["t_send_start"] + 1.0
    t_off = run.meta["t_send_start"] + run.meta["duration"] - window
    S = [s for s in S if (lo is None or lo < s[0] < hi - window) and t_on < s[0] < t_off]
    for t, _, _ in S:
        m = np.nonzero((kt > t) & (kt < t + window) & ~used)[0]
        if len(m):
            used[m[0]] = True
            lat.append((kt[m[0]] - t) * 1000)
            sims.append(K[m[0]][1])
        else:
            missed.append(round(t - S[0][0], 2))
    L = np.array(lat)
    return {"src": src, "sink": sink, "sig": sig, "n_src": len(S), "n_sink": len(K), "matched": len(L),
            "missed_at_s": missed[:10], "unmatched_sink": int((~used).sum()),
            "p50_ms": pct(L, 0.5), "p95_ms": pct(L, 0.95), "min_ms": pct(L, 0), "max_ms": pct(L, 1),
            "sd_ms": round(float(L.std()), 2) if len(L) else None,
            "sink_similarity_min": round(float(min(sims)), 3) if sims else None,
            "span_s": round(float(S[-1][0] - S[0][0]), 1) if S else 0}


def glitches(run, ref, sig):
    x, _, log = run.stream(ref)
    if x is None:
        return None
    marks = [e["index"] for e in log if e.get("event") == "marker"] or [j for j, _, _ in detect(x, sig, 0.3)]
    g = pilot_glitches(x.astype(np.float64), SIGS[sig]["pilot"], markers=marks)
    return {k: (float(v) if isinstance(v, np.floating) else v) for k, v in g.items()}


def cpu_events(run):
    """Exit/end events with CPU from every diagnostics file, labelled by file and tool mode."""
    out = []
    for f in sorted(glob.glob(os.path.join(run.d, "*.err")) + glob.glob(os.path.join(run.d, "pc", "*.err"))):
        mode = None
        for e in jl(f):
            if e.get("event") in ("start", "play", "capture_mic", "capture_system"):
                mode = e.get("mode") or e.get("event")
            if e.get("event") in ("exit", "end") and ("cpu_pct" in e or "cpu_pct_of_core" in e):
                out.append({"file": os.path.relpath(f, run.d), "mode": mode,
                            "cpu_pct": e.get("cpu_pct", e.get("cpu_pct_of_core")),
                            "underruns": e.get("underruns"), "overruns": e.get("overruns", e.get("overrun_bytes")),
                            "silent_frames": e.get("silent_frames")})
    return out


def harness_stats(run):
    res = {}
    for f in glob.glob(os.path.join(run.d, "*.log")) + glob.glob(os.path.join(run.d, "pc", "*.log")):
        ex = [e for e in jl(f) if e.get("event") == "exit"]
        st = [e for e in jl(f) if e.get("event") == "start"]
        if ex and st:
            res[os.path.relpath(f, run.d)] = {"cpu_s": ex[-1].get("cpu_s"), "wall_s": round(ex[-1]["t"] - st[0]["t"], 1),
                                              "excl": [e for e in jl(f) if e.get("event") == "exclude"]}
    return res


def endpoints(run):
    out = {}
    for w in ("before", "during", "after"):
        p = os.path.join(run.d, "pc", f"endpoints_{w}.json")
        if os.path.exists(p):
            try:
                d = json.load(open(p, encoding="utf-8-sig"))
            except ValueError:
                continue
            for e in d:
                if "G733" in e["name"] and e["flow"] == "render":
                    out[w] = {"mute": e["mute"], "volume": e["volume"], "default": e["default_console"]}
    return out


def transport(run):
    p = os.path.join(run.d, "send.summary.json")
    s = jl(p)
    if not s:
        return None
    s = s[-1]
    keep = ("seconds", "rx", "lost", "fec", "plc", "expand", "underrun", "rebuffer", "drop", "flushed", "late",
            "level_ms_mean", "target_ms_mean", "jitter_ms_mean", "cpu_pct_mean", "tx_kbps", "rx_kbps")
    agg = lambda a: {k: a.get(k) for k in keep if k in a} if a else None  # noqa: E731
    return {"mac": agg(s.get("local_agg")), "pc": agg(s.get("remote_agg")), "rtt_ms": s.get("rtt_ms")}


def rms_seconds(run, ref):
    x, _, _ = run.stream(ref)
    if x is None:
        return None
    n = len(x) // RATE
    r = np.sqrt(np.mean(x[:n * RATE].astype(np.float64).reshape(n, RATE) ** 2, axis=1) + 1e-30)
    return [round(float(20 * np.log10(v)), 1) for v in r]


def looptest(run):
    """Every A/B marker seen by each capture, relative to its first appearance, plus the energy
    in the 3 s after the first burst relative to the burst itself."""
    res = {}
    for ref in ("mac/src", "mac/sink", "pc/src", "pc/sink"):
        x, tm, _ = run.stream(ref)
        if x is None:
            continue
        r = {}
        for sig in ("A", "B"):
            d = detect(x.astype(np.float64), sig, thr=0.5, min_gap_s=0.05)  # echoes come ~150 ms apart
            if not d:
                r[sig] = {"n": 0}
                continue
            j0, s0, a0 = d[0]
            L = int(0.04 * RATE)
            e_burst = float(np.sum(x[j0:j0 + L].astype(np.float64) ** 2)) / L
            a, b = j0 + int(0.1 * RATE), min(len(x), j0 + int(3.0 * RATE))
            e_after = float(np.mean(x[a:b].astype(np.float64) ** 2)) if b > a else 0.0
            r[sig] = {"n": len(d), "first_t_mac": round(float(run.to_mac(ref, tm(j0))), 3),
                      "first_amp_dbfs": round(20 * np.log10(abs(a0) + 1e-12), 1),
                      "later": [{"dt_ms": round((j - j0) / RATE * 1000, 1), "sim": round(s, 2),
                                 "rel_db": round(20 * np.log10(abs(a) / abs(a0) + 1e-12), 1)} for j, s, a in d[1:6]],
                      "later_n": len(d) - 1,
                      "after_3s_vs_burst_db": round(10 * np.log10((e_after + 1e-20) / (e_burst + 1e-20)), 1)}
        res[ref] = r
    return res


def main():
    d = sys.argv[1]
    run = Run(d)
    m = run.meta
    out = {"name": m["name"], "scenario": m["scenario"], "desc": m["desc"], "clock": run.clock}
    out["latency"] = [latency(run, *p) for p in m["pairs"]]
    gl = {}
    for src, sink, sig in m["pairs"]:
        for ref in (src, sink):
            if not ref.endswith("inj"):
                gl[f"{ref}:{sig}"] = glitches(run, ref, sig)
    out["glitches"] = gl
    out["transport"] = transport(run)
    out["cpu"] = cpu_events(run)
    out["harness"] = harness_stats(run)
    out["pc_endpoint_g733"] = endpoints(run)
    if m["scenario"] == "s2noise":
        out["rms_per_s"] = {r: rms_seconds(run, r) for r in ("pc/src", "mac/jb", "mac/sink")}
    if m["scenario"] == "s3":
        x, tm, _ = run.stream("mac/mic")
        if x is not None:
            z = np.abs(np.array([np.mean(np.abs(np.fft.rfft(x[i:i + RATE] * np.hanning(RATE)))[1020:1031])
                                 for i in range(0, len(x) - RATE, RATE)]))
            out["mac_mic_1025Hz_band_per_s"] = [round(float(20 * np.log10(v + 1e-12)), 1) for v in z]
    if m.get("loopcheck"):
        out["loop_check"] = {
            "B_in_mac_src": len(detect(run.stream("mac/src")[0].astype(np.float64), "B", 0.3)),
            "A_in_pc_src": len(detect(run.stream("pc/src")[0].astype(np.float64), "A", 0.3)),
            "A_in_mac_src": len(detect(run.stream("mac/src")[0].astype(np.float64), "A", 0.3)),
            "B_in_pc_src": len(detect(run.stream("pc/src")[0].astype(np.float64), "B", 0.3)),
        }
    if m.get("looptest"):
        out["loop_test"] = looptest(run)
    json.dump(out, open(os.path.join(d, "result.json"), "w"), indent=1, default=float)
    print(json.dumps(out, indent=1, default=float))


if __name__ == "__main__":
    main()
