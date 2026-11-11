"""What-if on a measured run: frames that would miss their deadline for a fixed playout delay D (from encoder
output), i.e. completion delay (pacing + network + queue) + decode > D. Late frames are shown on arrival (judder),
not lost.   uv run -q --python 3.13 --with numpy dfloor.py NAME"""
import json, sys
import numpy as np
from analyze import analyze
out, D = analyze(sys.argv[1], quiet=True)
mf, rd, clock = D["mf"], D["rd"], D["clock"]
need = np.array([(d["d1"] - d["ac"]) / 1000 + (d["ac"] - clock(mf[f]["out"])) / 1000 for f, d in rd.items() if f in mf and not mf[f]["key"]])
mins = len(need) / 30 / 60
res = {f"D={x}": dict(late_per_min=round(float((need > x).sum() / mins), 2), late_pct=round(float((need > x).mean() * 100), 2)) for x in (15, 20, 25, 30, 40, 50, 60, 70)}
res["need_ms"] = {f"p{p}": round(float(np.percentile(need, p)), 1) for p in (50, 90, 95, 99, 99.9)}
print(json.dumps(res))
