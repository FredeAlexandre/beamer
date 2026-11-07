"""Level of a tone in raw f32le captures: overall RMS and the level in a narrow band around FREQ.

usage: uv run --with numpy python tone_level.py FREQ RATE CHANNELS file.f32 [file.f32 ...]
Skips the first 0.5 s of each file. Prints one JSON line per file.
"""
import json
import sys

import numpy as np

freq, rate, ch = float(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
for path in sys.argv[4:]:
    x = np.fromfile(path, dtype="<f4")
    x = x[: len(x) // ch * ch].reshape(-1, ch).mean(axis=1)[int(rate * 0.5):]
    if len(x) < rate // 2:
        print(json.dumps({"file": path, "error": "too short", "frames": len(x)}))
        continue
    rms = float(np.sqrt(np.mean(x.astype(np.float64) ** 2)))
    # Band level: windowed FFT, energy within +-20 Hz of freq, as an equivalent sine RMS.
    n = len(x)
    w = np.hanning(n)
    spec = np.abs(np.fft.rfft(x * w)) ** 2
    f = np.fft.rfftfreq(n, 1 / rate)
    band = (f > freq - 20) & (f < freq + 20)
    ref = ((f > freq - 300) & (f < freq - 100)) | ((f > freq + 100) & (f < freq + 300))
    norm = (np.sum(w**2)) * n / 2  # Parseval scaling for a windowed one-sided spectrum
    band_rms = float(np.sqrt(np.sum(spec[band]) / norm))
    ref_rms_per_bin = float(np.sqrt(np.mean(spec[ref]) * band.sum() / norm))
    db = lambda v: round(20 * np.log10(max(v, 1e-12)), 1)
    print(json.dumps({"file": path.rsplit("/", 1)[-1], "seconds": round(n / rate, 2), "rms_dbfs": db(rms),
                      "tone_band_dbfs": db(band_rms), "neighbour_band_dbfs": db(ref_rms_per_bin),
                      "tone_snr_db": round(db(band_rms) - db(ref_rms_per_bin), 1)}))
