"""Analyse one vprobe run (out/NAME.mac.npz + out/NAME.pc.npz) -> numbers + out/NAME.trace.npz.

    python analyze.py NAME [NAME ...] [--json]

Clock: pings (Mac t0, PC t1/t2, Mac t3); per 5 s window the lowest-RTT ping, then a linear fit of
the offset (PC - Mac) against Mac time (drift ~17 ppm). Delays are on the Mac clock.
The trace file keeps, per video packet: send time, frame, frag, nfrag, key, size, delay (nan = lost),
via_tcp; per frame: t_frame, size, key, npk, complete delay (nan = incomplete); per audio packet:
t_send, delay; per RR: PC send (Mac clock), Mac arrival. sim.py replays these traces.
"""
from __future__ import annotations

import json
import sys

import numpy as np


def load(name):
    m = np.load(f"out/{name}.mac.npz")
    p = np.load(f"out/{name}.pc.npz")
    return m, p


def clock_fit(pings, win=5.0):
    t0, t1, t2, t3 = pings.T
    rtt = (t3 - t0) - (t2 - t1)
    off = ((t1 - t0) + (t2 - t3)) / 2
    ok = rtt > 0
    t0, rtt, off = t0[ok], rtt[ok], off[ok]
    bins = np.floor((t0 - t0[0]) / win).astype(int)
    xs, ys = [], []
    for b in np.unique(bins):
        i = np.where(bins == b)[0]
        j = i[np.argmin(rtt[i])]
        xs.append(t0[j]); ys.append(off[j])
    xs, ys = np.array(xs), np.array(ys)
    a, b = np.polyfit(xs - xs[0], ys, 1)
    res = ys - (a * (xs - xs[0]) + b)
    keep = np.abs(res) < 3 * max(np.median(np.abs(res)), 1e-4)
    a, b = np.polyfit(xs[keep] - xs[0], ys[keep], 1)
    x0 = xs[0]
    f_mac = lambda t: a * (t - x0) + b  # offset at Mac time t

    def f_pc(t):  # offset at PC time t (solve t_mac = t - off(t_mac))
        o = f_mac(t - b)
        return f_mac(t - o)
    return f_pc, {"drift_ppm": a * 1e6, "rtt_min_ms": float(rtt.min() * 1e3),
                                          "rtt_p50_ms": float(np.median(rtt) * 1e3),
                                          "rtt_p99_ms": float(np.percentile(rtt, 99) * 1e3),
                                          "fit_resid_ms": float(np.median(np.abs(res[keep])) * 1e3)}


def pct(x, q):
    x = x[~np.isnan(x)]
    return float(np.percentile(x, q)) if len(x) else float("nan")


def runs_of(mask):
    """lengths of runs of True"""
    if not len(mask):
        return np.array([], int)
    d = np.diff(np.concatenate([[0], mask.astype(int), [0]]))
    s, e = np.where(d == 1)[0], np.where(d == -1)[0]
    return e - s


def analyze(name):
    m, p = load(name)
    meta = json.loads(str(m["meta"]))
    off, clk = clock_fit(m["pings"])
    rx = p["rx"]
    vlog, frames, alog = m["vlog"], m["frames"], m["alog"]
    out = {"name": name, "clock": clk, "meta": {k: meta.get(k) for k in
           ("rate", "fps", "key_every", "key_kb", "pace", "pace_bps", "burst_pk", "mtu", "trace", "duration", "audio", "video", "tcp")}}
    tr = {}
    # ---- video packets
    if len(vlog):
        seq = vlog[:, 0].astype(np.int64)
        tsend = vlog[:, 7]
        vr = rx[rx[:, 0] == 1]
        arr = np.full(seq.max() + 1, np.nan)
        # first arrival per seq (duplicates impossible here, but be safe)
        vseq = vr[:, 1].astype(np.int64)
        ta_mac = vr[:, 8] - off(vr[:, 8])
        order = np.argsort(ta_mac)[::-1]
        arr[vseq[order]] = ta_mac[order]
        a_v = arr[seq]
        dly = a_v - tsend
        lost = np.isnan(a_v)
        out["video_pk"] = {"sent": int(len(seq)), "lost": int(lost.sum()), "loss_pct": 100 * lost.mean(),
                           "burst_hist": {str(k): int(v) for k, v in zip(*np.unique(runs_of(lost), return_counts=True))},
                           "max_burst": int(runs_of(lost).max()) if lost.any() else 0,
                           "delay_ms_p50": 1e3 * pct(dly, 50), "p95": 1e3 * pct(dly, 95), "p99": 1e3 * pct(dly, 99),
                           "p1": 1e3 * pct(dly, 1), "max": 1e3 * np.nanmax(dly) if (~lost).any() else None,
                           "reorder": int(np.sum(np.diff(vr[:, 1]) < 0))}
        # ---- frames
        fid = vlog[:, 1].astype(np.int64)
        nf = int(frames[:, 0].max()) + 1
        complete = np.full(nf, -np.inf)
        anylost = np.zeros(nf, bool)
        np.maximum.at(complete, fid, np.where(lost, -np.inf, a_v))
        np.logical_or.at(anylost, fid, lost)
        tf = frames[:, 3]
        key = frames[:, 2].astype(bool)
        fd = np.where(anylost, np.nan, complete - tf)
        # last packet sent time per frame (pacing share)
        lastsend = np.full(nf, -np.inf)
        np.maximum.at(lastsend, fid, tsend)
        out["frames"] = {"n": nf, "lost": int(anylost.sum()), "lost_per_min": anylost.sum() / ((tf[-1] - tf[0]) / 60),
                         "delay_ms_p50": 1e3 * pct(fd, 50), "p95": 1e3 * pct(fd, 95), "p99": 1e3 * pct(fd, 99),
                         "p999": 1e3 * pct(fd, 99.9), "max": 1e3 * np.nanmax(fd),
                         "key_delay_ms_p50": 1e3 * pct(fd[key], 50), "key_p95": 1e3 * pct(fd[key], 95),
                         "key_p99": 1e3 * pct(fd[key], 99),
                         "p_delay_ms_p50": 1e3 * pct(fd[~key], 50), "p_p95": 1e3 * pct(fd[~key], 95),
                         "p_p99": 1e3 * pct(fd[~key], 99),
                         "pacing_ms_p50_key": 1e3 * pct((lastsend - tf)[key], 50),
                         "pacing_ms_p50_p": 1e3 * pct((lastsend - tf)[~key], 50),
                         "mean_kbps": float(frames[:, 1].sum() * 8 / (tf[-1] - tf[0] + 1 / 30) / 1e3),
                         "key_kb_mean": float(frames[key, 1].mean() / 1e3), "p_kb_mean": float(frames[~key, 1].mean() / 1e3)}
        for thr in (50, 100, 150, 200, 300):
            late = np.sum((fd > thr / 1e3) | np.isnan(fd))
            out["frames"][f"late_or_lost_{thr}ms_per_min"] = late / ((tf[-1] - tf[0]) / 60)
        fnet = np.where(anylost, np.nan, complete - lastsend)
        out["frames"].update({"net_ms_p50": 1e3 * pct(fnet, 50), "net_p95": 1e3 * pct(fnet, 95),
                              "net_p99": 1e3 * pct(fnet, 99), "net_p999": 1e3 * pct(fnet, 99.9),
                              "net_max": 1e3 * np.nanmax(fnet)})
        # network spike episodes: packets > 30 ms late, merged when < 0.5 s apart
        bad = np.where((dly > 0.030) | lost)[0]
        eps = []
        for i in bad:
            t = tsend[i] - tsend[0]
            v = 9.99 if lost[i] else dly[i]
            if eps and t - eps[-1]["end"] < 0.5:
                e = eps[-1]
                e["end"] = t; e["worst_ms"] = max(e["worst_ms"], 1e3 * v); e["pk"] += 1; e["lost"] += int(lost[i])
            else:
                eps.append({"t_s": t, "end": t, "worst_ms": 1e3 * v, "pk": 1, "lost": int(lost[i])})
        dur_min = (tsend[-1] - tsend[0]) / 60
        for e in eps:
            e["len_ms"] = 1e3 * (e.pop("end") - e["t_s"])
        out["net_spikes"] = {"per_min": len(eps) / dur_min,
                             "gt45ms_per_min": sum(e["worst_ms"] > 45 for e in eps) / dur_min,
                             "gt100ms_per_min": sum(e["worst_ms"] > 100 for e in eps) / dur_min,
                             "longest_ms": max([e["len_ms"] for e in eps], default=0),
                             "worst_ms": max([e["worst_ms"] for e in eps], default=0)}
        out["spike_list"] = [e for e in eps if e["worst_ms"] > 45]
        # spike timeline: seconds where some frame took > 60 ms
        sec = np.floor(tf - tf[0]).astype(int)
        worst = np.full(sec.max() + 1, 0.0)
        np.maximum.at(worst, sec, np.nan_to_num(fd, nan=9.99))
        ev = []
        bad = worst > 0.060
        i = 0
        while i < len(bad):
            if bad[i]:
                j = i
                while j + 1 < len(bad) and bad[j + 1]:
                    j += 1
                lost_n = int(anylost[(sec >= i) & (sec <= j)].sum())
                ev.append({"t_s": i, "len_s": j - i + 1, "worst_ms": round(1e3 * min(worst[i:j + 1].max(), 9.99), 1),
                           "lost_frames": lost_n})
                i = j + 1
            else:
                i += 1
        out["spikes"] = ev
        tr.update(v_send=tsend, v_frame=fid, v_frag=vlog[:, 2], v_nfrag=vlog[:, 3], v_key=vlog[:, 4], v_size=vlog[:, 5],
                  v_delay=dly, f_t=tf, f_size=frames[:, 1], f_key=frames[:, 2], f_npk=frames[:, 4], f_delay=fd)
    # ---- audio
    if len(alog):
        ar = rx[rx[:, 0] == 2]
        arr = np.full(int(alog[:, 0].max()) + 1, np.nan)
        arr[ar[:, 1].astype(np.int64)] = ar[:, 8] - off(ar[:, 8])
        aseq = alog[:, 0].astype(np.int64)
        ad = arr[aseq] - alog[:, 1]  # from the scheduled time: includes any sender blocking (TCP)
        al = np.isnan(ad)
        out["audio"] = {"sent": int(len(aseq)), "lost": int(al.sum()), "loss_pct": 100 * al.mean(),
                        "delay_ms_p50": 1e3 * pct(ad, 50), "p95": 1e3 * pct(ad, 95), "p99": 1e3 * pct(ad, 99),
                        "p999": 1e3 * pct(ad, 99.9), "max": 1e3 * np.nanmax(ad),
                        "jitter_p97_ms": 1e3 * (pct(ad, 97) - pct(ad, 1)),
                        "spikes_gt20ms_per_min": float(np.sum(np.diff((ad > pct(ad, 1) + 0.020).astype(int)) == 1)
                                                       / ((alog[-1, 2] - alog[0, 2]) / 60))}
        tr.update(a_send=alog[:, 2], a_delay=ad)
    # ---- receiver reports (PC -> Mac)
    rr = m["rrlog"]
    if len(rr):
        rr_send_mac = rr[:, 0] - off(rr[:, 0])
        rd = rr[:, 1] - rr_send_mac
        out["rr"] = {"n": int(len(rr)), "delay_ms_p50": 1e3 * pct(rd, 50), "p95": 1e3 * pct(rd, 95), "p99": 1e3 * pct(rd, 99)}
        tr.update(rr_send=rr_send_mac, rr_arr=rr[:, 1])
    np.savez_compressed(f"out/{name}.trace.npz", **tr)
    return out


def r(x):
    if isinstance(x, float):
        return round(x, 2)
    if isinstance(x, dict):
        return {k: r(v) for k, v in x.items()}
    if isinstance(x, list):
        return [r(v) for v in x]
    return x


if __name__ == "__main__":
    for n in [a for a in sys.argv[1:] if not a.startswith("--")]:
        res = r(analyze(n))
        with open(f"out/{n}.summary.json", "w") as f:
            json.dump(res, f, indent=1)
        sp = res.pop("spikes", [])
        sl = res.pop("spike_list", [])
        print("network spike episodes > 45 ms:")
        for e in sl:
            print("  ", e)
        print(json.dumps(res, indent=1))
        print(f"frame-delay seconds (> 60 ms incl. pacing): {len(sp)} episodes")
