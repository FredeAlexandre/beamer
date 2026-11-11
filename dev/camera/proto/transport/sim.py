"""Replay measured Wi-Fi traces (analyze.py -> out/NAME.trace.npz) under video recovery policies.

    python sim.py NAME [NAME ...]                 # policy x playout-delay table
    python sim.py NAME --stall                     # what happens in the longest stall

Channel: every simulated packet sent at time t takes the delay/loss of a measured video packet sent
at (about) the same time: a cursor walks the measured packets in order and re-syncs to the measured
send time when it drifts by more than 30 ms, so bursts of loss and delay spikes are replayed where
they happened. Feedback (NACK, PLI) sent by the PC at t reaches the Mac with the delay of the next
receiver report the Mac actually got (sent at or after t).

Receiver model (both machines run real-time):
- a frame is complete when all its packets (originals or repairs) have arrived; it is decodable when
  complete and its reference chain is intact (previous frame decodable, or it is an IDR);
- frame f is shown at t_f + D (t_f = encoder output time, D = playout delay) if decodable by then;
  a late but complete frame is still decoded (it is a reference) but never shown late: the display
  skips to the newest frame (no latency debt after a stall);
- frozen time = display ticks without a new frame; degraded time (intra refresh only) = frames shown
  with concealment artefacts until the refresh wave has passed.

Policies (any combination; '+'):
  idr2s   periodic IDR every 2 s, nothing else (what the measured runs did)
  pli     loss detected (seq gap) -> PLI; the sender encodes an IDR on its next frame (one in flight
          at a time, re-armed after 300 ms)
  nack    loss detected -> NACK; the sender resends at once from a 1 s history; re-NACK after
          max(2 x RTT, 15 ms) while the frame's deadline (t_f + D + nack_extra) has not passed
  fec<k>  one XOR parity per k packets of a frame (sent right after the group): repairs 1 loss/group
  ltr     like pli, but the sender answers with an LTR refresh: a P-frame that references the last
          long-term reference the PC acknowledged (VideoToolbox low-latency mode,
          ForceLTRRefresh + AcknowledgedLTRTokens); size 1.4x a P-frame (C1 t_ltr.jsonl: 12.3-14.2 KB vs P mean 10.1 KB; forced IDR 120 KB)
  ir<n>   periodic intra refresh over n frames instead of IDRs (no big frames; artefacts after a loss
          until a full wave has passed)
"""
from __future__ import annotations

import json
import math
import random
import sys

import numpy as np

FRAG = 1200 - 52
OVH = 52


class Channel:
    def __init__(self, tr):
        s = tr["v_send"]
        o = np.argsort(s)
        self.s = s[o]
        self.d = tr["v_delay"][o]
        self.i = 0
        self.rr_send = tr["rr_send"]
        self.rr_d = tr["rr_arr"] - tr["rr_send"]
        self.t0 = self.s[0]
        self.t1 = self.s[-1]

    def fwd(self, t):
        """arrival time or None (lost) of a packet sent at t (call in send-time order)."""
        i = self.i
        if i >= len(self.s) or abs(self.s[i] - t) > 0.030:
            i = int(np.searchsorted(self.s, t))
        i = min(i, len(self.s) - 1)
        self.i = i + 1
        d = self.d[i]
        return None if math.isnan(d) else t + d

    def fwd_at(self, t):
        """random access (repairs): the measured packet nearest in time."""
        i = min(int(np.searchsorted(self.s, t)), len(self.s) - 1)
        d = self.d[i]
        return None if math.isnan(d) else t + d

    def rev(self, t):
        k = int(np.searchsorted(self.rr_send, t))
        if k >= len(self.rr_send):
            return t + 0.005
        return t + max(self.rr_d[k], 0.0) + (self.rr_send[k] - t)


def frame_sizes(n, rate, fps, policy, key_kb, seed):
    rng = random.Random(seed)
    per = rate / 8 / fps
    if "idr2s" in policy:
        ke = 60
        p_mean = (per * ke - key_kb * 1000) / (ke - 1)
    else:
        ke = 0
        p_mean = per
    sig = 0.34
    mu = math.log(p_mean) - sig ** 2 / 2
    sizes = [max(300, int(rng.lognormvariate(mu, sig))) for _ in range(n)]
    return sizes, ke, p_mean


def simulate(tr, meta, policy, D, nack_extra=0.100, seed=1, ltr_factor=1.4, key_kb=None, pace=None, bucket=None):
    pol = set(policy.split("+"))
    rate, fps = meta["rate"], meta["fps"]
    key_kb = meta["key_kb"] if key_kb is None else key_kb
    pace_override = pace
    pace = meta["pace"] if pace is None else pace
    ch = Channel(tr)
    fiv = 1 / fps
    t_start = ch.t0 + 0.5
    n = int((ch.t1 - t_start - 1.0) * fps)
    sizes, ke, p_mean = frame_sizes(n, rate, fps, policy, key_kb, seed)
    fec_k = next((int(p[3:]) for p in pol if p.startswith("fec")), 0)
    ir_n = next((int(p[2:]) for p in pol if p.startswith("ir")), 0)
    pace_bps = pace * rate if pace > 0 else 0
    bucket_cap = 0.0
    if pace_override is None and meta.get("pace_bps"):
        pace_bps = meta["pace_bps"]
        bucket_cap = meta.get("burst_pk", 0) * 1200 * 8.0
    if bucket is not None:  # (pace_bps, burst_pk) override
        pace_bps, bucket_cap = bucket[0], bucket[1] * 1200 * 8.0
    tokens, t_tok = bucket_cap, None
    rtt = float(np.median(ch.rr_d)) * 2 + 0.002
    rto = max(2 * rtt, 0.015)

    # event-driven: frames generated in time order; feedback arrivals change later frames
    pli_at = []        # times PLIs reach the sender
    pli_armed_until = -1.0
    complete = [math.inf] * n   # time the frame became complete at the PC
    is_key = [False] * n
    pace_free = t_start
    bytes_sent = 0
    sent_pk = 0
    rtx_pk = 0
    fec_pk = 0
    broken_since = math.inf
    # Process frame by frame; feedback from frame f's losses affects frames sent after the
    # feedback arrives, which is always > t_f (causal). Repairs for frame f are computed inline.
    for f in range(n):
        tf = t_start + f * fiv
        key = f == 0 or (ke and f % ke == 0)
        forced = False
        while pli_at and pli_at[0] <= tf:
            pli_at.pop(0)
            forced = True
        if forced and "ltr" in pol and not key:
            size = int(ltr_factor * p_mean)  # LTR refresh: resets the chain, P-frame sized
            key = True
        else:
            key = bool(key) or forced
            size = int(key_kb * 1000 * (0.9 + 0.2 * random.Random(f).random())) if key else sizes[f]
        is_key[f] = key
        npk = max(1, math.ceil(size / FRAG))
        # packet plan: data packets (+ parity after each group of fec_k)
        plan = []
        left = size
        for k in range(npk):
            pl = min(FRAG, left)
            left -= pl
            plan.append(("d", k, pl + OVH))
            if fec_k and ((k + 1) % fec_k == 0 or k == npk - 1):
                plan.append(("p", k // fec_k, FRAG + OVH))
        arrivals = {}
        par = {}
        last_send = tf
        for kind, k, plen in plan:
            if pace_bps and bucket_cap:
                ts = max(pace_free, tf)
                if t_tok is None:
                    t_tok = ts
                tokens = min(bucket_cap, tokens + (ts - t_tok) * pace_bps)
                t_tok = ts
                if tokens < plen * 8:
                    ts += (plen * 8 - tokens) / pace_bps
                    tokens = plen * 8
                    t_tok = ts
                tokens -= plen * 8
                pace_free = ts
            elif pace_bps:
                ts = max(pace_free, tf)
                pace_free = ts + plen * 8 / pace_bps
            else:
                ts = tf
            last_send = ts
            a = ch.fwd(ts)
            bytes_sent += plen
            sent_pk += 1
            if kind == "d":
                arrivals[k] = (ts, a)
            else:
                fec_pk += 1
                par[k] = a
        # FEC repair: a group with exactly one loss and its parity -> recovered when the last of
        # the others + parity is in
        got = {k: a for k, (ts, a) in arrivals.items()}
        if fec_k:
            for g in range((npk + fec_k - 1) // fec_k):
                ks = range(g * fec_k, min(npk, (g + 1) * fec_k))
                miss = [k for k in ks if got[k] is None]
                if len(miss) == 1 and par.get(g) is not None:
                    got[miss[0]] = max([par[g]] + [got[k] for k in ks if k != miss[0]])
        # loss detection time: arrival of the next received packet (seq gap); for tail losses the
        # next frame's first packet (~1 frame later) or the packet after it
        missing = [k for k in range(npk) if got[k] is None]
        if missing:
            later = [a for a in got.values() if a is not None]
            t_detect = {}
            for k in missing:
                after = [got[j] for j in range(k + 1, npk) if got[j] is not None]
                t_detect[k] = min(after) if after else ch.fwd_at(tf + fiv) or (tf + fiv + 0.010)
            if "nack" in pol:
                deadline = tf + D + nack_extra
                for k in missing:
                    t = t_detect[k]
                    tries = 0
                    # a hole of more than 64 packets is a stall: no NACK storm, the PLI/LTR fallback handles it
                    while t < deadline and tries < 3 and len(missing) <= 64:
                        tries += 1
                        t_srv = ch.rev(t)
                        a = ch.fwd_at(t_srv)
                        rtx_pk += 1
                        bytes_sent += FRAG + OVH
                        if a is not None:
                            got[k] = a
                            break
                        t += rto
            still = [k for k in missing if got[k] is None]
            if still and ("pli" in pol or "ltr" in pol):
                t_pli = (tf + D + nack_extra) if "nack" in pol else min(t_detect[k] for k in still)
                if t_pli >= pli_armed_until:
                    pli_at.append(ch.rev(t_pli))
                    pli_at.sort()
                    pli_armed_until = t_pli + 0.300
        if all(got[k] is not None for k in range(npk)):
            complete[f] = max(got.values())
        # receiver chain state (for PLI repeats): an incomplete frame breaks it, a complete IDR /
        # LTR refresh mends it. While broken, the PC repeats the PLI every 300 ms (it knows it
        # cannot decode). Intra refresh never breaks the chain (concealment).
        if not ir_n:
            if complete[f] == math.inf:
                broken_since = min(broken_since, tf)
            elif key:
                broken_since = math.inf
            if broken_since < math.inf and ("pli" in pol or "ltr" in pol):
                t_rep = tf + fiv  # noticed by the next frame's arrival at the latest
                if t_rep >= pli_armed_until:
                    pli_at.append(ch.rev(t_rep))
                    pli_at.sort()
                    pli_armed_until = t_rep + 0.300
    # decode / display
    shown = 0
    frozen_ticks = []
    degraded = 0
    dec_prev = -math.inf
    chain_ok = True
    last_loss = -10 ** 9
    disp = np.zeros(n, bool)
    adapt = "adapt" in pol
    hist, hist_i, Dcur, Dsum = [], 0, D, 0.0
    for f in range(n):
        tf = t_start + f * fiv
        c = complete[f]
        if is_key[f] and c < math.inf:
            chain_ok = True
            last_loss = -10 ** 9
            dec = c
        elif ir_n:
            # intra refresh: decodable whenever complete (concealed if the chain had a hole)
            if c == math.inf:
                last_loss = f
                dec = math.inf
            else:
                dec = max(c, dec_prev if dec_prev < math.inf else c)
        else:
            if c == math.inf:
                chain_ok = False
            dec = max(c, dec_prev) if chain_ok else math.inf
        if dec < math.inf:
            dec_prev = dec
        if adapt:
            # adaptive playout: p99 of the network part of the last ~10 s of complete frames + 8 ms,
            # rises at once, falls by at most 1 ms per frame; clamped [Dmin, 200 ms]
            while hist_i < f and complete[hist_i] <= tf:
                if complete[hist_i] < math.inf:
                    hist.append(complete[hist_i] - (t_start + hist_i * fiv))
                hist_i += 1
            if len(hist) > 30 and f % 15 == 0:
                want = float(np.percentile(hist[-300:], 99)) + 0.008
                Dcur = want if want > Dcur else max(want, Dcur - 0.015)
                Dcur = min(max(Dcur, D), 0.200)
            Dsum += Dcur
        if dec <= tf + (Dcur if adapt else D):
            disp[f] = True
            shown += 1
            if ir_n and f - last_loss <= ir_n:
                degraded += 1
    dur_min = n * fiv / 60
    nd = ~disp
    extra = {"D_mean_ms": round(Dsum / n * 1e3, 1)} if adapt else {}
    # freeze episodes = runs of non-displayed frames
    d = np.diff(np.concatenate([[0], nd.astype(int), [0]]))
    runs = np.where(d == -1)[0] - np.where(d == 1)[0]
    data_bytes = sum(sizes[f] for f in range(n))
    return {
        "policy": policy, "D_ms": round(D * 1e3),
        "frozen_ms_per_min": round(nd.sum() * fiv * 1e3 / dur_min, 1),
        "freezes_per_min": round(len(runs) / dur_min, 2),
        "freezes_gt100ms_per_min": round(np.sum(runs * fiv > 0.1) / dur_min, 2),
        "longest_freeze_ms": round(runs.max() * fiv * 1e3) if len(runs) else 0,
        "degraded_ms_per_min": round(degraded * fiv * 1e3 / dur_min, 1),
        "idr_per_min": round(sum(is_key) / dur_min, 2),
        "kbps": round(bytes_sent * 8 / (n * fiv) / 1e3),
        "rtx_pk_per_min": round(rtx_pk / dur_min, 1),
        "fec_overhead_pct": round(100 * fec_pk / max(1, sent_pk - fec_pk), 1),
        **extra,
        "frame_p99_ms": round(float(np.percentile([complete[f] - (t_start + f * fiv) for f in range(n) if complete[f] < math.inf], 99)) * 1e3, 1),
    }


def overlay(tr, kind, seed=7):
    """Stress overlays on a measured trace (labelled as such in the report):
    iid<p>      independent loss, p in percent (e.g. iid0.5)
    ge<p>       Gilbert-Elliott bursts, average loss p %, mean burst 6 packets (~ a 10-20 ms Wi-Fi hole)
    stall<s>    every 5 min, s seconds of total loss (packets sent in it are lost)
    hold<s>     every 5 min, packets sent in s seconds are held by the AP and all delivered at its end
    """
    tr = dict(tr)
    d = tr["v_delay"].copy()
    s = tr["v_send"]
    rng = np.random.default_rng(seed)
    if not kind:
        return tr
    for k in kind.split("+"):
        if k.startswith("iid"):
            d[rng.random(len(d)) < float(k[3:]) / 100] = np.nan
        elif k.startswith("ge"):
            p = float(k[2:]) / 100
            r = 1 / 6                      # leave bad state: mean burst 6
            q = p * r / (1 - p)            # enter bad state
            bad = False
            u = rng.random(len(d))
            for i in range(len(d)):
                bad = (u[i] >= r) if bad else (u[i] < q)
                if bad:
                    d[i] = np.nan
        elif k.startswith(("stall", "hold")):
            ln = float(k[5:] if k.startswith("stall") else k[4:])
            t0 = s[0] + 60
            while t0 + ln < s[-1]:
                m = (s >= t0) & (s < t0 + ln)
                if k.startswith("stall"):
                    d[m] = np.nan
                else:
                    d[m] = np.maximum(d[m], (t0 + ln) - s[m] + 0.003)
                t0 += 300
    tr["v_delay"] = d
    return tr


POLICIES = ["idr2s", "pli", "ltr", "ir30", "ir30+pli", "nack", "nack+pli", "nack+ltr", "fec5+pli", "fec10+pli",
            "nack+pli+fec10"]


def main():
    names = [a for a in sys.argv[1:] if not a.startswith("--")]
    ov = next((a.split("=", 1)[1] for a in sys.argv if a.startswith("--overlay=")), "")
    pols = next((a.split("=", 1)[1].split(",") for a in sys.argv if a.startswith("--policies=")), POLICIES)
    Ds = [float(x) / 1e3 for x in next((a.split("=", 1)[1].split(",") for a in sys.argv if a.startswith("--D=")),
                                        ["40", "60", "80", "100", "150"])]
    kkb = next((float(a.split("=", 1)[1]) for a in sys.argv if a.startswith("--key-kb=")), None)
    pc = next((float(a.split("=", 1)[1]) for a in sys.argv if a.startswith("--pace=")), None)
    bk = next((tuple(float(x) for x in a.split("=", 1)[1].split(",")) for a in sys.argv if a.startswith("--bucket=")), None)
    allres = {}
    for name in names:
        tr = overlay(dict(np.load(f"out/{name}.trace.npz")), ov)
        meta = json.load(open(f"out/{name}.summary.json"))["meta"]
        res = []
        for pol in pols:
            for D in Ds:
                r = simulate(tr, meta, pol, D, key_kb=kkb, pace=pc, bucket=bk)
                res.append(r)
                r["overlay"] = ov
                print(name, json.dumps(r))
        allres[name] = res
    json.dump(allres, open("out/sim_" + "_".join(names) + (("_" + ov) if ov else "") + ".json", "w"), indent=1)


if __name__ == "__main__":
    main()
