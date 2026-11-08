# SPDX-License-Identifier: GPL-3.0-only
"""Differential check of the C++ jitter buffer against the Phase 1 Python reference.

    BA_SIM_DUMP=/tmp/x native/audio/build/tests/engine_tests jb_gilbert
    uv run --python 3.13 --with numpy --with pynacl python native/audio/tests/jb_crosscheck.py /tmp/x

Replays the dumped arrival trace through dev/audio/proto/transport/media.py `JitterBuffer` (with
the engine's 10 ms floor) and compares the per-pull level and target with the engine's.
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "dev/audio/proto/transport"))
import media as M  # noqa: E402
import opus  # noqa: E402

pfx = sys.argv[1]
lines = Path(pfx + ".arrivals").read_text().split("\n")
block, seconds = int(lines[0].split()[0]), float(lines[0].split()[1])
arr = [(float(a), int(b)) for a, b in (l.split() for l in lines[1:] if l)]
ref = np.loadtxt(pfx + ".pulls")

enc = opus.Encoder(channels=1, application="voip", bitrate=32000, fec=True, loss_perc=10, dtx=False, complexity=9)
t = np.arange(480 * 1000) / 48000
env = 0.55 + 0.45 * np.sin(2 * np.pi * 3 * t)
x = (0.12 * env * sum(np.sin(2 * np.pi * k * 180 * t) / k for k in range(1, 13))).astype(np.float32)
frames = [enc.encode(x[k * 480:(k + 1) * 480].reshape(-1, 1)) for k in range(1000)]

jb = M.JitterBuffer(M.C_OPUS, 1, 480, min_ms=10.0)
ai = 0
out = []
ts0 = 0xFFF00000
for i in range(len(ref)):
    now = i * block / 48000
    while ai < len(arr) and arr[ai][0] <= now:
        p = M.Packet()
        k = arr[ai][1]
        p.ptype, p.flags, p.codec, p.stream, p.seq = M.MEDIA, M.F_FEC, M.C_OPUS, 1, k + 1
        p.ts, p.nsamp, p.channels = (ts0 + k * 480) & 0xFFFFFFFF, 480, 1
        p.payload, p.size, p.arrival = frames[k % 1000], 100, arr[ai][0]
        jb.push(p)
        ai += 1
    n0 = len(jb.levels)
    jb.pull(block, now)
    lv = jb.levels[-1] if len(jb.levels) > n0 else jb.level_ms()
    out.append((lv, jb.target_ms))
out = np.array(out)
dl = np.abs(out[:, 0] - ref[:, 1])
dt = np.abs(out[:, 1] - ref[:, 2])
print(f"pulls {len(ref)}: level |diff| p50 {np.median(dl):.3f} p99 {np.quantile(dl, .99):.3f} max {dl.max():.3f} ms;"
      f" target |diff| max {dt.max():.3f} ms")
print(f"python stats: {dict(jb.stats)}")
print(f"mean level py {out[:, 0].mean():.2f} c++ {ref[:, 1].mean():.2f}; mean target py {out[:, 1].mean():.2f} c++ {ref[:, 2].mean():.2f}")
