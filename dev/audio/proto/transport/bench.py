"""Offline measurements (no network): codec CPU/bitrate/algorithmic delay, resampler quality.

    uv run --with pynacl --with numpy bench.py
"""
from __future__ import annotations

import json
import platform
import time

import numpy as np

import media as M
import opus

RATE = 48000


def speechlike(n, ch, seed=1):
    """Deterministic voice-like test signal: harmonic pulse train with pitch glide, formant-ish
    filtering and syllable envelope, plus a little noise."""
    rng = np.random.default_rng(seed)
    t = np.arange(n) / RATE
    f0 = 140 + 40 * np.sin(2 * np.pi * 0.7 * t)
    ph = 2 * np.pi * np.cumsum(f0) / RATE
    x = sum(np.sin(k * ph) / k for k in range(1, 25))
    env = np.clip(np.sin(2 * np.pi * 2.5 * t), 0, 1) ** 0.5
    x = 0.2 * x * env + 0.003 * rng.standard_normal(n)
    return np.repeat(x.astype(np.float32)[:, None], ch, axis=1)


def codec_bench(codec, ch, frame_ms, bitrate, app, fec, seconds=10):
    fr = int(frame_ms * RATE / 1000)
    x = speechlike(seconds * RATE, ch)
    if codec == "opus":
        enc = opus.Encoder(channels=ch, bitrate=bitrate, application=app, fec=fec)
        dec = opus.Decoder(channels=ch)
        look = enc.get(opus.GET_LOOKAHEAD)
    else:
        enc, dec, look = M.PcmEncoder(), M.PcmDecoder(ch), 0
    te = td = 0.0
    nbytes = 0
    out = []
    for i in range(0, len(x) - fr + 1, fr):
        t0 = time.perf_counter()
        p = enc.encode(x[i:i + fr])
        t1 = time.perf_counter()
        y = dec.decode(p, fr)
        t2 = time.perf_counter()
        te += t1 - t0
        td += t2 - t1
        nbytes += len(p)
        out.append(y)
    nfr = len(out)
    y = np.concatenate(out)
    # algorithmic delay: cross-correlate input/output
    a, b = x[:RATE * 2, 0], y[:RATE * 2, 0]
    c = np.correlate(b[:RATE], a[:RATE - 2000], mode="valid")
    lag = int(np.argmax(c))
    pkt = nbytes / nfr
    return {"codec": codec, "ch": ch, "frame_ms": frame_ms, "bitrate_set": bitrate if codec == "opus" else None,
            "app": app if codec == "opus" else None, "fec": fec if codec == "opus" else None,
            "payload_B": round(pkt, 1), "payload_kbps": round(pkt * 8 * 1000 / frame_ms / 1000, 1),
            "wire_kbps": round((pkt + M.OVERHEAD_UDP) * 8 * 1000 / frame_ms / 1000, 1),
            "enc_us": round(te * 1e6 / nfr, 1), "dec_us": round(td * 1e6 / nfr, 1),
            "cpu_pct_1core": round((te + td) * 100 / seconds, 2),
            "lookahead_ms": round(look * 1000 / RATE, 2), "measured_delay_ms": round(lag * 1000 / RATE, 2)}


def resampler_quality():
    res = []
    for f in (440.0, 1000.0, 5000.0, 10000.0, 16000.0):
        for ppm in (100.0, 500.0):
            n = RATE * 2
            t = np.arange(n + 64)
            x = (0.5 * np.sin(2 * np.pi * f * t / RATE)).astype(np.float32)[:, None]
            rs = M.Resampler(1)
            step = 1 + ppm * 1e-6
            rs.feed(x)
            m = int((n - 32) / step)
            y = rs.out(m, step)[:, 0].astype(np.float64)
            # ideal: sample i at input position (start_pos + i*step) relative to x index
            p0 = M.Resampler.TAPS // 2 - 1 - M.Resampler.TAPS  # history zeros precede x
            pos = p0 + np.arange(m) * step
            ideal = 0.5 * np.sin(2 * np.pi * f * pos / RATE)
            sl = slice(200, m - 200)
            err = y[sl] - ideal[sl]
            snr = 10 * np.log10(np.mean(ideal[sl] ** 2) / np.mean(err ** 2))
            res.append({"freq_hz": f, "ppm": ppm, "snr_db": round(float(snr), 1)})
    return res


def jb_cpu():
    """Cost of the jitter buffer + resampler per 10 ms pull (Python), opus decode included."""
    ch, fr = 1, 480
    enc = opus.Encoder(channels=ch, bitrate=32000)
    sealer = M.Sealer(b"k" * 32, 1)
    op = M.Opener(b"k" * 32)
    jb = M.JitterBuffer(M.C_OPUS, ch, fr)
    x = speechlike(RATE * 5, ch)
    pk = []
    for i, k in enumerate(range(0, len(x) - fr + 1, fr)):
        pk.append(sealer.seal(M.MEDIA, enc.encode(x[k:k + fr]), 0, M.C_OPUS, k, fr, ch))
    t0 = time.perf_counter()
    for i, p in enumerate(pk):
        q = op.open(p, i * 0.01)
        jb.push(q)
        if i > 3:
            jb.pull(480, i * 0.01)
    dt = time.perf_counter() - t0
    return {"per_frame_us_open_push_pull": round(dt * 1e6 / len(pk), 1)}


def crypto_selftest():
    """Per-packet AEAD and replay window behaviour."""
    k = bytes(range(32))
    se, op = M.Sealer(k, 7), M.Opener(k)
    pk = [se.seal(M.MEDIA, b"x" * 40, 0, M.C_OPUS, i * 480, 480, 1) for i in range(300)]
    r = {}
    r["in_order_ok"] = sum(op.open(p, 0) is not None for p in pk[:200])
    r["replay_rejected"] = sum(op.open(p, 0) is None for p in pk[190:200])
    late_ok = op.open(pk[250], 0) is not None and op.open(pk[240], 0) is not None  # reordered within window
    r["reordered_within_window_ok"] = late_ok
    r["older_than_window_rejected"] = op.open(pk[100], 0) is None
    bad = bytearray(pk[260]); bad[30] ^= 1
    r["tampered_payload_rejected"] = op.open(bytes(bad), 0) is None
    bad = bytearray(pk[261]); bad[17] ^= 1  # header (AD) seq byte
    r["tampered_header_rejected"] = op.open(bytes(bad), 0) is None
    r["wrong_key_rejected"] = M.Opener(bytes(32)).open(pk[262], 0) is None
    return r


def main():
    info = {"platform": platform.platform(), "machine": platform.machine(), "python": platform.python_version(),
            "opus": opus.version()}
    rows = []
    for codec, ch, fms, br, app, fec in [
        ("opus", 1, 10, 32000, "voip", True), ("opus", 1, 20, 32000, "voip", True),
        ("opus", 1, 10, 32000, "voip", False), ("opus", 1, 5, 32000, "voip", True),
        ("opus", 1, 10, 32000, "lowdelay", False), ("opus", 2, 10, 96000, "audio", False),
        ("opus", 2, 20, 96000, "audio", False), ("opus", 2, 10, 128000, "lowdelay", False),
        ("pcm", 1, 10, 0, "", False), ("pcm", 2, 5, 0, "", False)]:
        rows.append(codec_bench(codec, ch, fms, br, app, fec))
    out = {"info": info, "crypto": crypto_selftest(), "codecs": rows, "resampler": resampler_quality(), "jb": jb_cpu()}
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
