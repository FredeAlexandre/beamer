"""C4 analysis: joins the Mac sender log, the PC receiver log and the browser's per-frame log of one run.

    uv run -q --python 3.13 --with numpy analyze.py NAME [--json]

Clock: the receiver's pings (PC unix us t1/t2, Mac host ns at the Mac) give, per 5 s window, the lowest-RTT
sample; a linear fit maps Mac ns -> PC unix us (drift in ppm, residuals). The browser's clock is
performance.timeOrigin + now() on the same PC (checked against Date.now() in C4K lines).

Per frame (joined by the global frame number carried in the code strip):
  encode   = encoder output - capture              (Mac)
  pacing   = last packet sent - encoder output      (Mac)
  network  = last packet arrived (PC) - last packet sent (Mac, mapped)
  queue    = decode start - frame complete          (PC: waits for earlier frames)
  decode   = MF decode + readback                   (PC)
  hold     = ring write - decode end                (PC: playout deadline)
  vcam+app = browser frame callback - ring write    (Frame Server + Chrome)
  total    = browser frame callback - capture
"""
from __future__ import annotations

import json
import re
import sys
from collections import Counter
from pathlib import Path

import numpy as np

RAW = Path(__file__).parent / "out" / "raw"


def jl(p):
    out = []
    if not p.exists():
        return out
    for line in p.read_text(errors="replace").splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return out


def pct(a, ps=(50, 95, 99)):
    a = np.asarray([x for x in a if x is not None], float)
    if not len(a):
        return {}
    d = {f"p{p}": round(float(np.percentile(a, p)), 2) for p in ps}
    d.update(n=int(len(a)), mean=round(float(a.mean()), 2), max=round(float(a.max()), 2), min=round(float(a.min()), 2))
    return d


def browser(name):
    p = RAW / f"{name}.chrome.log"
    frames, cycles, clocks = [], [], []
    if not p.exists():
        return frames, cycles, clocks
    for line in p.read_text(errors="replace").splitlines():
        m = re.search(r'"(C4[FCK]) (.*)", source:', line)
        if not m:
            continue
        tag, body = m.group(1), m.group(2).replace('\\"', '"')
        try:
            o = json.loads(body)
        except json.JSONDecodeError:
            continue
        if tag == "C4F":
            frames += o
        elif tag == "C4C":
            cycles.append(o)
        else:
            clocks.append(o)
    return frames, cycles, clocks


class Clock:
    def __init__(self, pings):
        t1 = np.array([p["t1"] for p in pings], float)
        t2 = np.array([p["t2"] for p in pings], float)
        mac = np.array([p["mac"] for p in pings], float) / 1000.0
        rtt = t2 - t1
        win = (t2 // 5e6).astype(np.int64)
        keep = []
        for w in np.unique(win):
            idx = np.where(win == w)[0]
            keep.append(idx[np.argmin(rtt[idx])])
        keep = np.array(keep)
        x, y = mac[keep], (t1[keep] + t2[keep]) / 2
        self.x0 = x[0]
        if len(keep) >= 3:
            self.b, self.a = np.polyfit(x - self.x0, y, 1)
        else:
            self.b, self.a = 1.0, float(np.median(y - x)) + self.x0
            self.a = float(np.median(y - (x - self.x0)))
        res = y - (self.a + self.b * (x - self.x0))
        self.info = dict(samples=int(len(keep)), drift_ppm=round((self.b - 1) * 1e6, 2), resid_ms_max=round(float(np.abs(res).max()) / 1000, 3),
                         rtt_min_ms=round(float(rtt.min()) / 1000, 2), rtt_p50_ms=round(float(np.median(rtt)) / 1000, 2))

    def __call__(self, mac_ns):  # -> PC unix us
        return self.a + self.b * (mac_ns / 1000.0 - self.x0)


def load(name):
    mac = jl(RAW / f"{name}.mac.jsonl")
    rcv = jl(RAW / f"{name}.recv.jsonl")
    bf, cyc, clk = browser(name)
    # Chrome's performance.timeOrigin + now() (QPC) drifts against the system clock (Date.now(), the same clock as
    # GetSystemTimePreciseAsFileTime in camrecv): ~-25 ppm here. Fit Date - perf over the C4K samples, correct every t.
    ck = [(c["t"], c["date_minus_perf_ms"] * 1000) for c in clk if "date_minus_perf_ms" in c and "t" in c]
    if ck:
        if len(ck) >= 3:
            b, a = np.polyfit([x - ck[0][0] for x, _ in ck], [y for _, y in ck], 1)
        else:
            b, a = 0.0, float(np.mean([y for _, y in ck]))
        fix = lambda t: t + a + b * (t - ck[0][0])
        bf = [[k, ts, fix(t)] for k, ts, t in bf]
        for c in cyc:
            for key in ("t_call", "t_res", "first_any", "first_mac", "t_close", "t_close_done"):
                if c.get(key):
                    c[key] = fix(c[key])
    clock = Clock([e for e in rcv if e.get("ev") == "ping"])
    return mac, rcv, bf, cyc, clk, clock


def freezes(times_us, thresh_ms=100):
    """gaps between consecutive delivered Mac frames: frozen time = gap - 33.3 ms for gaps > thresh."""
    t = np.asarray(times_us, float)
    if len(t) < 2:
        return [], 0.0
    g = np.diff(t) / 1000
    ev = [(float(t[i]), float(g[i])) for i in np.where(g > thresh_ms)[0]]
    return ev, float(sum(x - 33.3 for _, x in ev))


def analyze(name, quiet=False):
    mac, rcv, bf, cyc, clk, clock = load(name)
    mf = {e["f"]: e for e in mac if e.get("ev") == "f"}
    rd = {e["f"]: e for e in rcv if e.get("ev") == "d"}
    rw = {e["f"]: e for e in rcv if e.get("ev") == "w"}
    ro = {e["f"]: e["psnr"] for e in rcv if e.get("ev") == "o"}
    out = dict(name=name, clock=clock.info)
    # ---- browser integrity
    kinds = Counter()
    last, ooo, dup, skipped = -1, 0, 0, 0
    seen = {}
    mac_times = []
    for k, ts, t in bf:
        if k >= 0:
            kinds["mac"] += 1
            if last >= 0:
                if k < last:
                    ooo += 1
                elif k == last:
                    dup += 1
                elif k > last + 1:
                    skipped += k - last - 1
            if k not in seen:
                seen[k] = (t, ts)
                mac_times.append(t)
            last = max(last, k)
        else:
            kinds[{-1: "pattern", -2: "placeholder", -3: "helper_stamp", -9: "unreadable"}.get(k, str(k))] += 1
    # strip time vs Mac log (catches a strip that decodes to a valid but wrong value)
    strip_bad = sum(1 for f, (t, ts) in seen.items() if f in mf and ts != (mf[f]["cap"] // 100000) % (1 << 32))
    out["browser"] = dict(kinds=dict(kinds), out_of_order=ooo, repeated=dup, skipped_frames=skipped, strip_time_mismatch=strip_bad,
                          oracle_checked=len(ro), oracle_below_30db=sum(1 for v in ro.values() if v < 30),
                          oracle_psnr=pct(list(ro.values()), (1, 50)))
    # ---- per-frame stages
    st = {k: [] for k in ("encode", "pacing", "network", "queue", "decode", "hold", "vcam_app", "total", "recv_to_app", "playout_D")}
    rows = []
    for f, (tb, ts) in seen.items():
        m, d, w = mf.get(f), rd.get(f), rw.get(f)
        if not (m and d and w):
            continue
        r = dict(f=f, encode=(m["out"] - m["cap"]) / 1e6, pacing=(m["s1"] - m["out"]) / 1e6, network=(d["ac"] - clock(m["s1"])) / 1000,
                 queue=(d["d0"] - d["ac"]) / 1000, decode=(d["d1"] - d["d0"]) / 1000, hold=(w["wr"] - d["d1"]) / 1000,
                 vcam_app=(tb - w["wr"]) / 1000, total=(tb - clock(m["cap"])) / 1000, recv_to_app=(tb - d["ac"]) / 1000, playout_D=d["D"],
                 key=m["key"], st=d["st"], t=tb)
        rows.append(r)
        if m["key"]:
            continue
        for k in st:
            st[k].append(r[k])
    out["stages_ms"] = {k: pct(v) for k, v in st.items()}
    keys = [r for r in rows if r["key"]]
    if keys:
        out["idr_frames_total_ms"] = [round(r["total"], 1) for r in keys][:20]
    fr, frozen = freezes(mac_times)
    dur = (mac_times[-1] - mac_times[0]) / 1e6 if len(mac_times) > 1 else 0
    out["delivery"] = dict(seconds=round(dur, 1), fps=round(len(mac_times) / dur, 2) if dur else 0, freezes_over_100ms=len(fr),
                           frozen_ms=round(frozen), frozen_ms_per_min=round(frozen / dur * 60, 1) if dur else 0,
                           longest_gap_ms=round(max([g for _, g in fr], default=0)))
    sts = Counter(e["st"] for e in rd.values())
    out["receiver"] = dict(status=dict(sts), lost_frames=sum(1 for e in rcv if e.get("ev") == "lost_frame"),
                           plis=sum(1 for e in rcv if e.get("ev") == "pli"), nacks=sum(1 for e in rcv if e.get("ev") == "nack"),
                           holes=[(e["seq0"], e["n"]) for e in rcv if e.get("ev") == "hole"][:50],
                           repaired_pk=sum(e.get("rep", 0) for e in rd.values()))
    stats = [e for e in rcv if e.get("ev") == "stats"]
    if stats:
        out["receiver"]["cpu_pct"] = pct([e["cpu_pct"] for e in stats[1:-1]], (50, 95))
        if "cpu_cyc_pct" in stats[0]:
            out["receiver"]["cpu_cyc_pct"] = pct([e["cpu_cyc_pct"] for e in stats[1:-1]], (50, 95))
            out["receiver"]["oracle_cpu_pct"] = pct([e["oracle_cpu_pct"] for e in stats[1:-1]], (50,))
        out["receiver"]["ws_mb"] = [stats[1]["ws_mb"] if len(stats) > 1 else None, stats[-1]["ws_mb"], max(e["ws_mb"] for e in stats)]
        out["receiver"]["priv_mb"] = [stats[1]["priv_mb"] if len(stats) > 1 else None, stats[-1]["priv_mb"], max(e["priv_mb"] for e in stats)]
        out["receiver"]["D"] = pct([e["D"] for e in stats], (50,))
    ms = [e for e in mac if e.get("ev") == "stats" and e.get("streaming")]
    if ms:
        out["mac"] = dict(cpu_pct=pct([e["cpu_pct"] - e.get("synth_cpu_pct", 0) for e in ms[1:-1]], (50, 95)),
                          synth_cpu_pct=pct([e.get("synth_cpu_pct", 0) for e in ms[1:-1]], (50,)), rss_mb=[ms[0]["rss_mb"], ms[-1]["rss_mb"]],
                          tx_kbps=pct([e["tx_kbps"] for e in ms[1:-1]], (50,)), rtx=ms[-1]["rtx"])
    mframes = list(mf.values())
    if mframes:
        out["mac"] = out.get("mac", {})
        out["mac"]["frames"] = len(mframes)
        out["mac"]["idr"] = sum(1 for e in mframes if e["key"])
        out["mac"]["refresh"] = sum(1 for e in mframes if e["refresh"])
        out["mac"]["p_bytes"] = pct([e["size"] for e in mframes if not e["key"] and not e["refresh"]], (50, 95))
        out["mac"]["idr_bytes"] = pct([e["size"] for e in mframes if e["key"]], (50,))
        out["mac"]["refresh_bytes"] = pct([e["size"] for e in mframes if e["refresh"] and not e["key"]], (50, 95))
    # stability over time: 5-minute windows (latency, playout delay, receiver memory)
    if rows:
        t0 = min(r["t"] for r in rows)
        win = {}
        for r in rows:
            if not r["key"]:
                win.setdefault(int((r["t"] - t0) // 300e6), []).append(r)
        out["by_5min"] = [dict(w=k, total_p50=round(float(np.percentile([r["total"] for r in v], 50)), 1),
                               total_p99=round(float(np.percentile([r["total"] for r in v], 99)), 1),
                               hold_p50=round(float(np.percentile([r["hold"] for r in v], 50)), 1),
                               D_p50=round(float(np.percentile([r["playout_D"] for r in v], 50)), 1),
                               D_max=round(float(max(r["playout_D"] for r in v)), 1), n=len(v)) for k, v in sorted(win.items())]
        live = [e for e in stats if e.get("shown")]
        out["receiver_mem_series"] = [(round((e["t"] - t0) / 60e6, 1), e["ws_mb"], e["priv_mb"]) for e in live][::30]
        offs = [(e["t"], e["off"]) for e in rcv if e.get("ev") == "stats"]
        if len(offs) > 2:
            out["live_offset_drift_ms"] = round((offs[-1][1] - offs[1][1]) / 1000, 2)
    out["browser_clock"] = [round(c.get("date_minus_perf_ms", 0), 2) for c in clk if "date_minus_perf_ms" in c][::6]
    if not quiet:
        print(json.dumps(out, indent=1))
    return out, dict(mac=mac, rcv=rcv, bf=bf, cyc=cyc, clock=clock, mf=mf, rd=rd, rw=rw, seen=seen, rows=rows, mac_times=mac_times)


if __name__ == "__main__":
    analyze(sys.argv[1])
