# SPDX-License-Identifier: GPL-3.0-only
"""Dry run of the E2E analysis on C4's recordings (dev/camera/proto/e2e/out/raw, git-ignored, on this Mac):
the same functions the cases use (analysis.py), fed with C4's browser frames (code strip + Chrome clock
corrected for its -25 ppm drift), compared with C4's own published numbers (out/*.json).

    uv run --python 3.13 --with numpy dev/camera/e2e/run.py replay-c4 [q1_720 q2_cycles40 q3_loss720 q4_soak ...]
"""
from __future__ import annotations

import json
from pathlib import Path

import analysis as A

C4 = Path(__file__).resolve().parents[1] / "proto" / "e2e" / "out"
RAW = C4 / "raw"


def published(name):
    p = C4 / f"{name}.json"
    return json.loads(p.read_text()) if p.exists() else {}


def latency(name):
    frames, cycles, mf, clock = A.c4_load(RAW, name)
    lat = A.latency_ms(frames, skip_s=3.0)
    # C4's total excludes key frames (IDR); with skip_s the start IDR is out, the rest are P/LTR frames
    st = A.pct([v for _, v in lat])
    integ = A.integrity(frames)
    eps = A.frozen_episodes(frames, 100.0)
    nf = frames.new_frames()
    dur = (nf[-1][0] - nf[0][0]) / 60 if len(nf) > 1 else 0
    # strip time vs the Mac's own log: same capture time to 0.1 ms (catches a wrong unwrap or clock map)
    mism = sum(1 for t, f, cap in nf if f in mf and abs(cap * 1e9 - mf[f]["cap"]) > 2e5)
    p = published(name)
    out = {"name": name, "clock": clock.info, "latency_ms": st, "c4_total_ms": p.get("stages_ms", {}).get("total"),
           "integrity": integ, "c4_browser": {k: p.get("browser", {}).get(k) for k in ("out_of_order", "strip_time_mismatch", "kinds")},
           "frozen_100ms": {"n": len(eps), "ms_per_min": round(sum(e[2] for e in eps) / dur, 1) if dur else None},
           "c4_delivery": p.get("delivery"), "strip_vs_mac_log_mismatch": mism}
    wins = {}
    t0 = nf[0][0] if nf else 0
    for t, v in lat:
        wins.setdefault(int((t - t0) // 300), []).append(v)
    out["p50_by_5min"] = [round(float(A.np.percentile(v, 50)), 2) for k, v in sorted(wins.items()) if len(v) > 3000]
    out["c4_by_5min"] = [w.get("total_p50") for w in p.get("by_5min", [])]
    return out


def cycles(name):
    frames, cyc, mf, clock = A.c4_load(RAW, name)
    rows = []
    for c in cyc:
        if not c.get("t_call_mac"):
            continue
        s = A.start_of_read(A.Frames([r for r in frames.rows if c["t_call_mac"] - 0.05 <= r[0] <= c.get("t_close_mac", 1e18)]),
                            c["t_call_mac"])
        rows.append(s)
    st = A.pct([r["first_ms"] for r in rows], (50, 95))
    p = published(name)
    return {"name": name, "first_ms": st, "c4_first_picture": p.get("summary", {}).get("first_picture"),
            "black_before": sum(r["black_before"] for r in rows), "placeholder_before": [r["placeholder_before"] for r in rows][:5]}


def loss(name):
    frames, cyc, mf, clock = A.c4_load(RAW, name)
    rcv = A.jl(RAW / f"{name}.recv.jsonl")
    # C4's receiver injected the loss itself; its `t` is PC unix µs (the same clock as the browser's corrected time)
    inj = [e for e in rcv if e.get("ev") == "inject"]
    by = {}
    for i, e in enumerate(inj):
        t = clock.pc_to_mac(e["t"] / 1e6)
        nxt = clock.pc_to_mac(inj[i + 1]["t"] / 1e6) if i + 1 < len(inj) else t + 8
        worst, total = A.frozen_in(frames, t, min(nxt, t + 3.0))
        by.setdefault(f'{e["kind"]}{e["n"]}', []).append(total)
    mine = {k: A.pct(v, (50,)) for k, v in by.items()}
    p = published(name).get("by_kind", {})
    return {"name": name, "frozen_ms_by_kind": {k: {"replay": {x: mine[k].get(x) for x in ("p50", "max", "n")},
                                                    "c4": {x: (p.get(k, {}).get("frozen_ms") or {}).get(x) for x in ("p50", "max", "n")}}
                                                for k in mine}}


def main(names=()):
    names = list(names) or ["q1_720", "q1_1080", "q2_cycles40", "q2_cycles25", "q3_loss720", "q4_soak"]
    if not RAW.exists():
        print(f"no C4 recordings at {RAW} (git-ignored; they exist only on the Mac that ran C4)")
        return 2
    results = []
    for n in names:
        if not (RAW / f"{n}.chrome.log").exists():
            print(f"{n}: no recording, skipped")
            continue
        fn = cycles if n.startswith("q2") else loss if n.startswith("q3_loss") else latency
        r = fn(n)
        results.append(r)
        print(json.dumps(r, indent=1, default=str))
    out = Path(__file__).resolve().parent / "runs" / "replay_c4.json"
    out.parent.mkdir(exist_ok=True)
    out.write_text(json.dumps(results, indent=1, default=str))
    print(f"-> {out}")
    return 0
