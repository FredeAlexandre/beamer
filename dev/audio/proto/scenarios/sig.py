"""Test signals and their offline analysis for the audio scenarios (runs on the Mac and the PC).

Two signatures so that both directions can share one sink (scenario 5) and a loop is visible:
  A (Mac -> PC): pilot 1025 Hz, marker = 40 ms Hann-windowed up-chirp 2 -> 5 kHz
  B (PC -> Mac): pilot  625 Hz, marker = 40 ms Hann-windowed down-chirp 5 -> 2 kHz
Up and down chirps barely correlate with each other, so a matched filter for one ignores the other.

The pilots are chosen so that 10 ms holds a whole number of cycles plus a quarter (10.25 and
6.25): a lost or repeated 10 ms block (Opus frame, device period) shifts the pilot's phase by 90
degrees, which the lock-in below sees at once.
"""
from __future__ import annotations

import math

import numpy as np

RATE = 48000
SIGS = {
    "A": {"pilot": 1025.0, "f0": 2000.0, "f1": 5000.0},
    "B": {"pilot": 625.0, "f0": 5000.0, "f1": 2000.0},
}
CHIRP_S = 0.040


def chirp(sig: str) -> np.ndarray:
    s = SIGS[sig]
    n = int(RATE * CHIRP_S)
    t = np.arange(n) / RATE
    k = (s["f1"] - s["f0"]) / CHIRP_S
    return (np.hanning(n) * np.sin(2 * np.pi * (s["f0"] * t + 0.5 * k * t * t))).astype(np.float64)


class Injector:
    """Pilot + markers, rendered by absolute sample index (so it can be mixed into a live stream
    or written paced). Marker k starts at start + k*period + 37 ms*(k mod 7): the uneven spacing
    makes a wrong source/sink pairing impossible to miss."""

    def __init__(self, sig, pilot_amp=0.02, marker_amp=0.1, period=1.0, start=2.0, count=None):
        self.f = SIGS[sig]["pilot"]
        self.pa = pilot_amp
        self.tpl = chirp(sig) * marker_amp
        self.L = len(self.tpl)
        self.period, self.start, self.count = period, start, count

    def marker_index(self, k):
        return int(round(RATE * (self.start + k * self.period + 0.037 * (k % 7))))

    def render(self, s0, n):
        idx = np.arange(s0, s0 + n)
        x = self.pa * np.sin(2 * np.pi * self.f * idx / RATE) if self.pa else np.zeros(n)
        starts = []
        # markers that overlap [s0, s0+n)
        k = max(0, int((s0 / RATE - self.start - 1.0) / self.period) - 1)
        while True:
            if self.count is not None and k >= self.count:
                break
            m = self.marker_index(k)
            if m >= s0 + n:
                break
            if m + self.L > s0:
                a, b = max(m, s0), min(m + self.L, s0 + n)
                x[a - s0:b - s0] += self.tpl[a - m:b - m]
                if s0 <= m < s0 + n:
                    starts.append(m)
            k += 1
        return x.astype(np.float32), starts


# ---------------------------------------------------------------- analysis (offline)

def detect(x: np.ndarray, sig: str, thr=0.4, min_gap_s=0.3):
    """Matched filter. Returns [(start_index, similarity, amplitude)] with similarity the cosine
    between the window and the template (1 = exact shape, independent of level)."""
    tpl = chirp(sig)
    L = len(tpl)
    if len(x) < 2 * L:
        return []
    nfft = 1 << int(math.ceil(math.log2(len(x) + L)))
    # zero-phase band limit to the chirps' band first, so pilots and room noise outside it do
    # not dilute the similarity
    X = np.fft.rfft(x.astype(np.float64), nfft)
    f = np.fft.rfftfreq(nfft, 1 / RATE)
    X[(f < 1500) | (f > 5500)] = 0
    x = np.fft.irfft(X, nfft)[:len(x)]
    c = np.fft.irfft(X * np.conj(np.fft.rfft(tpl, nfft)), nfft)[:len(x) - L + 1]
    cs = np.concatenate([[0.0], np.cumsum(x * x)])
    e = cs[L:] - cs[:-L]
    sim = c / (np.linalg.norm(tpl) * np.sqrt(np.maximum(e, 1e-20)))
    sim[e < 1e-14] = 0
    cand = np.nonzero(sim > thr)[0]
    out = []
    gap = int(min_gap_s * RATE)
    if len(cand):
        # clusters of nearby candidates -> their best point
        splits = np.nonzero(np.diff(cand) > gap)[0] + 1
        for grp in np.split(cand, splits):
            j = grp[np.argmax(sim[grp])]
            amp = float(c[j] / np.dot(tpl, tpl))
            out.append((int(j), float(sim[j]), amp))
    return out


def lockin(x: np.ndarray, f: float, W=480):
    """Complex pilot amplitude per W-sample window (Hann), phase referenced to absolute sample
    index, so a steady pilot gives a constant phase."""
    n = len(x) // W
    if n == 0:
        return np.zeros(0, complex)
    w = np.hanning(W)
    idx = np.arange(n * W).reshape(n, W)
    ref = np.exp(-2j * np.pi * f * idx / RATE)
    return 2 * (x[:n * W].reshape(n, W) * w * ref).sum(1) / w.sum()


def pilot_glitches(x: np.ndarray, f: float, W=480, phase_thr=0.3, drop_db=-6.0, present_amp=1e-5,
                   skip_s=2.0, markers=()):
    """Glitch windows of a pilot: phase step > phase_thr rad between consecutive windows, or
    amplitude more than 6 dB below the running median. Only where the pilot is present (running
    median above 30 % of the run's 90th percentile), skipping skip_s after it appears and before it ends."""
    z = lockin(x, f, W)
    if len(z) < 10:
        return {"windows": 0}
    a = np.abs(z)
    # running median over ~1 s
    k = int(RATE / W)
    pad = np.pad(a, (k // 2, k // 2), mode="edge")
    med = np.array([np.median(pad[i:i + k]) for i in range(len(a))])
    present = med > max(present_amp, 0.3 * float(np.quantile(a, 0.9)))
    if not present.any():
        return {"windows": 0, "present": False}
    on = np.nonzero(present)[0]
    s = int(skip_s * RATE / W)
    lo, hi = on[0] + s, on[-1] - s
    dphi = np.zeros(len(z))
    dphi[1:] = np.abs(np.angle(z[1:] * np.conj(z[:-1])))
    bad = ((dphi > phase_thr) | (20 * np.log10(np.maximum(a, 1e-12) / np.maximum(med, 1e-12)) < drop_db)) & present
    use = np.zeros(len(z), bool)
    use[lo:hi] = True
    use &= present
    # windows a marker overlaps (and the next 20 ms) are not judged: the codec spends its bits on
    # the chirp there and the pilot's phase wobbles, which is the test signal, not a glitch
    for j in markers:
        use[max(0, j // W - 1):min(len(use), (j + int(CHIRP_S * RATE)) // W + 3)] = False
    b = bad & use
    events = int(np.sum(b[1:] & ~b[:-1]) + (1 if b[0] else 0))
    t_ev = (np.nonzero(b[1:] & ~b[:-1])[0] + 1) * W / RATE
    return {"windows": int(use.sum()), "seconds": round(float(use.sum() * W / RATE), 1),
            "glitch_windows": int(b.sum()), "glitch_events": int(events),
            "events_per_min": round(events / max(1e-9, use.sum() * W / RATE / 60), 2),
            "pilot_dbfs": round(float(20 * np.log10(np.median(a[use]) + 1e-12)), 1) if use.any() else None,
            "dphi_p99": round(float(np.quantile(dphi[use], 0.99)), 3) if use.any() else None,
            "glitch_at_s": [round(float(t), 2) for t in t_ev[:20]]}


def time_map(chunks: np.ndarray, window_s=1.0):
    """chunks: (n, 2) of (index after the chunk's last frame, read time). A chunk can only be read
    late, never early, so the lower envelope of t - end/RATE (min over +-window) is the best
    estimate of the stream's timeline. Returns f(index) -> time."""
    e, t = chunks[:, 0], chunks[:, 1]
    off = t - e / RATE
    # rolling min over +-window (in samples of the chunk index)
    order = np.argsort(e)
    e, off = e[order], off[order]
    half = window_s * RATE
    lo = np.searchsorted(e, e - half)
    hi = np.searchsorted(e, e + half, side="right")
    env = np.array([off[a:b].min() for a, b in zip(lo, hi)])

    def f(i):
        return np.asarray(i) / RATE + np.interp(i, e, env)
    f.lateness_ms = float(np.median(off - env) * 1000)
    return f


def clock_fit(pings, keep=0.3):
    """pings: [(t_mac, rtt, off)] with off = PC clock - Mac clock (NTP formula on the ping
    exchange). Keeps the lowest-RTT fraction (least queueing, so least asymmetry) and fits
    off = a + b t. Returns (f(t_mac) -> off, info)."""
    P = np.array(pings, float)
    thr = np.quantile(P[:, 1], keep)
    Q = P[P[:, 1] <= thr]
    b, a = np.polyfit(Q[:, 0], Q[:, 2], 1)
    res = Q[:, 2] - (a + b * Q[:, 0])
    info = {"pings": len(P), "used": len(Q), "rtt_min_ms": round(float(P[:, 1].min() * 1000), 2),
            "rtt_p50_ms": round(float(np.median(P[:, 1]) * 1000), 2),
            "slope_ppm": round(float(b * 1e6), 2), "fit_resid_sd_ms": round(float(res.std() * 1000), 3)}
    return (lambda t: a + b * np.asarray(t)), info


def pct(v, q):
    return round(float(np.quantile(v, q)), 1) if len(v) else None
