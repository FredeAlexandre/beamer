"""Device loop latency: play clicks into an output device, capture them from an input device
(e.g. CABLE Input -> CABLE Output on the PC), report the latency as the transport sees it
(output callback that wrote the click -> input callback that delivered it) and as PortAudio
timestamps see it (DAC time -> ADC time).

    python devloop.py OUT_DEVICE IN_DEVICE [--seconds 20] [--block 480] [--channels 1]
"""
from __future__ import annotations

import argparse
import json
import threading
import time

import numpy as np
import sounddevice as sd

from peer import _sd_device

RATE = 48000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("inp")
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--block", type=int, default=480)
    ap.add_argument("--channels", type=int, default=1)
    ap.add_argument("--latency", default="low")
    a = ap.parse_args()
    od, idev = _sd_device(a.out, "output"), _sd_device(a.inp, "input")
    period = RATE // 2
    n = 96
    click = (0.5 * np.hanning(n) * np.sin(2 * np.pi * 2000 * (np.arange(n) - 48) / RATE + np.pi / 2)).astype(np.float32)
    lock = threading.Lock()
    emitted = []  # (k, wall time of callback + offset, dac time)
    st = {"pos": 0, "in_pos": 0, "cool": 0}
    res = []

    def ocb(out, frames, t, status):
        now = time.perf_counter()
        pos = st["pos"]
        out[:] = 0
        idx = np.arange(pos, pos + frames)
        ph = idx % period
        m = ph < n
        if m.any():
            out[m, :] = click[ph[m]][:, None]
            first = int(np.nonzero(ph == 48)[0][0]) if (ph == 48).any() else None
            if first is not None:
                with lock:
                    emitted.append((now + first / RATE, t.outputBufferDacTime + first / RATE))
        st["pos"] = pos + frames

    def icb(indata, frames, t, status):
        now = time.perf_counter()
        x = np.abs(indata[:, 0])
        hits = np.nonzero(x > 0.1)[0]
        if len(hits) and now > st["cool"]:
            i = int(hits[0])
            j = i + int(np.argmax(x[i:i + 192]))
            with lock:
                if emitted:
                    tw, tdac = emitted[-1]
                    res.append(((now + j / RATE) - tw, (t.inputBufferAdcTime + j / RATE) - tdac))
            st["cool"] = now + 0.25

    os_ = sd.OutputStream(device=od, samplerate=RATE, channels=a.channels, dtype="float32",
                          blocksize=a.block, latency=a.latency, callback=ocb)
    is_ = sd.InputStream(device=idev, samplerate=RATE, channels=a.channels, dtype="float32",
                         blocksize=a.block, latency=a.latency, callback=icb)
    is_.start()
    os_.start()
    time.sleep(a.seconds)
    os_.stop()
    is_.stop()
    r = np.array(res[2:]) * 1000 if len(res) > 2 else np.zeros((1, 2))
    print(json.dumps({"out": sd.query_devices(od)["name"], "in": sd.query_devices(idev)["name"],
                      "block": a.block, "pa_out_latency_ms": round(os_.latency * 1000, 2),
                      "pa_in_latency_ms": round(is_.latency * 1000, 2), "n": len(r),
                      "callback_to_callback_ms": {"p50": round(float(np.median(r[:, 0])), 2),
                                                  "min": round(float(r[:, 0].min()), 2),
                                                  "max": round(float(r[:, 0].max()), 2)},
                      "dac_to_adc_ms": {"p50": round(float(np.median(r[:, 1])), 2),
                                        "min": round(float(r[:, 1].min()), 2),
                                        "max": round(float(r[:, 1].max()), 2)}}))


if __name__ == "__main__":
    main()
