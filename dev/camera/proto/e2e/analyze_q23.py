"""C4 Q2 (on demand) and Q3 (loss recovery, IDR pacing) analyses on top of analyze.py.

    uv run -q --python 3.13 --with numpy analyze_q23.py cycles NAME
    uv run -q --python 3.13 --with numpy analyze_q23.py loss NAME
    uv run -q --python 3.13 --with numpy analyze_q23.py idr NAME
"""
from __future__ import annotations

import csv
import json
import sys
from collections import Counter, defaultdict

import numpy as np

from analyze import RAW, analyze, pct


def cycles(name):
    out, D = analyze(name, quiet=True)
    clock, rcv, mac, mf, rd, rw = D["clock"], D["rcv"], D["mac"], D["mf"], D["rd"], D["rw"]
    ev = lambda kind, src: [e for e in src if e.get("ev") == kind]
    dem = ev("demand", rcv)
    rows = []
    cyc = [c for c in D["cyc"] if "t_call" in c]
    for i, c in enumerate(cyc):
        t0 = c["t_call"]
        t1 = cyc[i + 1]["t_call"] if i + 1 < len(cyc) else 1e30
        within = lambda t: t0 - 1e5 <= t < t1
        on = next((e for e in dem if e["streaming"] and within(e["t"])), None)
        off = next((e for e in dem if not e["streaming"] and on and e["t"] > on["t"] and e["t"] < t1), None)
        stx = next((e for e in ev("start_tx", rcv) if within(e["t"])), None)
        srx = next((e for e in ev("state_rx", rcv) if within(e["t"]) and e["running"]), None)
        mstart = next((e for e in ev("start_rx", mac) if within(clock(e["t"]))), None)
        vtc = next((e for e in ev("vt_created", mac) if within(clock(e["t"]))), None)
        tc = off["t"] if off else 1e30
        mstop = next((e for e in ev("stop_rx", mac) if tc - 5e3 <= clock(e["t"]) < t1), None)
        mstopped = next((e for e in ev("stopped", mac) if tc - 5e3 <= clock(e["t"]) < t1), None)
        f = c.get("first_f")
        # the stream's first frame (its IDR) carries the start-up path; the browser may first show a later frame
        sess_frames = sorted(x for x in mf if within(clock(mf[x]["cap"])))
        f0 = sess_frames[0] if sess_frames else f
        m, d, w = mf.get(f0), rd.get(f0), rw.get(f0)
        r = dict(c=c["c"], ok=c.get("ok"), first_f=f, idr=bool(m and m["key"]), first_shown_is_idr=f == f0)
        if on and stx and mstart and vtc and m and d and w and c.get("first_mac"):
            r.update(
                gum_to_source_start=(on["src_t"] - t0) / 1000,
                source_to_helper=(on["t"] - on["src_t"]) / 1000,
                helper_to_mac=(clock(mstart["t"]) - stx["t"]) / 1000,
                vt_create=(vtc["t"] - mstart["t"]) / 1e6,
                first_capture=(m["cap"] - vtc["t"]) / 1e6,
                encode=(m["out"] - m["cap"]) / 1e6,
                pacing=(m["s1"] - m["out"]) / 1e6,
                network=(d["ac"] - clock(m["s1"])) / 1000,
                decode_to_ring=(w["wr"] - d["ac"]) / 1000,
                idr_ring_to_first_browser_frame=(c["first_mac"] - w["wr"]) / 1000,
                first_picture=(c["first_mac"] - t0) / 1000,
                gum_resolved=(c["t_res"] - t0) / 1000,
                first_any_frame=(c["first_any"] - t0) / 1000 if c.get("first_any") else None,
                idr_bytes=m["size"], idr_npk=m["npk"],
            )
        if off and mstopped and c.get("t_close"):
            r.update(close_to_helper=(off["t"] - c["t_close"]) / 1000,
                     helper_to_mac_stop_rx=(clock(mstop["t"]) - off["t"]) / 1000 if mstop else None,
                     helper_to_mac_stopped=(clock(mstopped["t"]) - off["t"]) / 1000,
                     close_to_mac_stopped=(clock(mstopped["t"]) - c["t_close"]) / 1000,
                     mac_stop_work=(mstopped["t"] - mstopped["t0"]) / 1e6)
        rows.append(r)
    keys = sorted({k for r in rows for k in r if k not in ("c", "ok", "first_f", "idr", "first_shown_is_idr")}) if rows else []
    summ = {k: pct([r.get(k) for r in rows if r.get(k) is not None], (50, 95)) for k in keys}
    print(json.dumps(dict(cycles=len(rows), ok=sum(1 for r in rows if r.get("ok")), stream_started_with_idr=sum(1 for r in rows if r.get("idr")), browser_first_frame_is_the_idr=sum(1 for r in rows if r.get("first_shown_is_idr")),
                          summary=summ, browser=out["browser"], rows=rows), indent=1))


def loss(name):
    out, D = analyze(name, quiet=True)
    rcv, mac, clock, bf = D["rcv"], D["mac"], D["clock"], D["bf"]
    mt = np.array(D["mac_times"], float)
    inj = [e for e in rcv if e.get("ev") == "inject"]
    by = defaultdict(list)
    rows = []
    for i, e in enumerate(inj):
        t0 = e["t"]
        t1 = inj[i + 1]["t"] if i + 1 < len(inj) else t0 + 8e6
        win = lambda t: t0 - 1e5 <= t < min(t1, t0 + 8e6)
        # browser: biggest gap between delivered Mac frames that ends after the injection
        idx = np.where((mt[1:] > t0) & (mt[:-1] < t0 + 3e6))[0]
        gaps = np.diff(mt)[idx] / 1000 if len(idx) else np.array([33.3])
        gap = float(gaps.max())
        frozen = float(sum(g - 33.3 for g in gaps if g > 50))
        ph = sum(1 for k, ts, t in bf if k == -2 and win(t))
        bad = sum(1 for k, ts, t in bf if k == -9 and win(t))
        r = dict(kind=f'{e["kind"]}{e["n"]}', t=t0, longest_gap_ms=round(gap, 1), frozen_ms=round(frozen, 1), placeholders=ph, unreadable=bad,
                 lost_frames=sum(1 for x in rcv if x.get("ev") == "lost_frame" and win(x["t"])),
                 plis=sum(1 for x in rcv if x.get("ev") == "pli" and win(x["t"])),
                 nacks=sum(1 for x in rcv if x.get("ev") == "nack" and win(x["t"])),
                 repaired=sum(x.get("rep", 0) for x in rcv if x.get("ev") == "d" and win(x["ac"])),
                 refresh=sum(1 for x in mac if x.get("ev") == "force_refresh" and win(clock(x["t"]))),
                 idr=sum(1 for x in mac if x.get("ev") == "force_idr" and win(clock(x["t"]))),
                 recovered_after_ms=next((round((x["t"] - t0) / 1000) for x in rcv if x.get("ev") == "recovered" and win(x["t"])), None))
        rows.append(r)
        by[r["kind"]].append(r)
    summ = {}
    for k, rs in by.items():
        summ[k] = dict(n=len(rs), frozen_ms=pct([r["frozen_ms"] for r in rs], (50, 95)), longest_gap_ms=pct([r["longest_gap_ms"] for r in rs], (50,)),
                       placeholders=sum(r["placeholders"] for r in rs), plis=sum(r["plis"] for r in rs), refresh=sum(r["refresh"] for r in rs),
                       idr=sum(r["idr"] for r in rs), lost_frames=sum(r["lost_frames"] for r in rs),
                       recovered_after_ms=pct([r["recovered_after_ms"] for r in rs if r["recovered_after_ms"] is not None], (50, 95)))
    print(json.dumps(dict(browser=out["browser"], delivery=out["delivery"], receiver={k: out["receiver"][k] for k in ("status", "lost_frames", "plis", "nacks")},
                          mac={k: out["mac"].get(k) for k in ("frames", "idr", "refresh", "refresh_bytes", "idr_bytes")}, by_kind=summ, rows=rows), indent=1))


def idr(name):
    out, D = analyze(name, quiet=True)
    rcv, mf, rd, clock = D["rcv"], D["mf"], D["rd"], D["clock"]
    holes = [(e["seq0"], e["n"], e["t"]) for e in rcv if e.get("ev") == "hole"]
    sess_of = {f: d["s"] for f, d in rd.items()}
    key, pf = [], []
    for f, m in mf.items():
        d = rd.get(f)
        if not d:
            continue
        lost = sum(min(d["seq0"] + d["npk"], s + n) - max(d["seq0"], s) for s, n, t in holes if s < d["seq0"] + d["npk"] and s + n > d["seq0"])
        r = dict(f=f, key=m["key"], npk=m["npk"], lost=lost, arrival_ms=(d["ac"] - clock(m["out"])) / 1000, send_ms=(m["s1"] - m["s0"]) / 1e6,
                 behind=m["behind"], size=m["size"])
        (key if m["key"] else pf).append(r)
    res = dict(idr_frames=len(key), idr_packets=sum(r["npk"] for r in key), idr_packets_lost=sum(r["lost"] for r in key),
               idr_frames_with_loss=sum(1 for r in key if r["lost"]), idr_bytes=pct([r["size"] for r in key], (50,)),
               idr_send_ms=pct([r["send_ms"] for r in key], (50, 95)), idr_arrival_ms=pct([r["arrival_ms"] for r in key], (50, 95, 99)),
               behind_idr_arrival_ms=pct([r["arrival_ms"] for r in pf if r["behind"]], (50, 95, 99)),
               p_arrival_ms=pct([r["arrival_ms"] for r in pf if not r["behind"]], (50, 95, 99)),
               p_packets=sum(r["npk"] for r in pf), p_packets_lost=sum(r["lost"] for r in pf),
               holes=Counter(n for s, n, t in holes), browser=out["browser"], delivery=out["delivery"], stages=out["stages_ms"]["total"])
    ap = RAW / f"{name}.recv.jsonl.audio.csv"
    if ap.exists():
        rows = list(csv.DictReader(ap.open()))
        seqs = np.array([int(r["seq"]) for r in rows])
        delay = np.array([(int(r["arr_us"]) - clock(int(r["send_ns"]))) / 1000 for r in rows])
        # audio packets sent while an IDR was being paced out
        win = sorted((mf[r["f"]]["s0"], mf[r["f"]]["s1"]) for r in key)
        sends = np.array([int(r["send_ns"]) for r in rows])
        during = np.zeros(len(rows), bool)
        for a, b in win:
            during |= (sends >= a) & (sends <= b + 20_000_000)
        res["audio"] = dict(received=int(len(seqs)), lost=int(seqs.max() - seqs.min() + 1 - len(np.unique(seqs))), delay_ms=pct(delay),
                            delay_during_idr_ms=pct(delay[during]), delay_else_ms=pct(delay[~during]))
    print(json.dumps(res, indent=1, default=str))


if __name__ == "__main__":
    {"cycles": cycles, "loss": loss, "idr": idr}[sys.argv[1]](sys.argv[2])
