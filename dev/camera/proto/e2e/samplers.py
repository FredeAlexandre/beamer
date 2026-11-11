"""CPU (% of one core) and memory of the processes sampled during a run: PC sampler.csv (camrecv, Frame Server
svchost, the C4 Chrome processes summed) and Mac macps.csv (camsend, VTEncoderXPCService).
    python3 samplers.py NAME"""
import csv, json, sys
from collections import defaultdict
from pathlib import Path
RAW = Path(__file__).parent / "out" / "raw"
name = sys.argv[1]
res = {}
p = RAW / f"{name}.sampler.csv"
if p.exists():
    by = defaultdict(lambda: defaultdict(list))  # name -> t -> rows
    for r in csv.DictReader(p.open()):
        by[r["name"]][int(r["t"])].append(r)
    for n, ts in by.items():
        t = sorted(ts)
        if len(t) < 3:
            continue
        # skip the first/last minute (start-up, teardown); sum per timestamp (Chrome has several processes, same pids)
        t = [x for x in t if t[0] + 60e3 <= x <= t[-1] - 60e3] or t
        cpu = lambda x: sum(float(r["cpu_s"]) for r in ts[x])
        pids = lambda x: {r["pid"] for r in ts[x]}
        if pids(t[0]) != pids(t[-1]):
            common = pids(t[0]) & pids(t[-1])
            cpu = lambda x, c=common: sum(float(r["cpu_s"]) for r in ts[x] if r["pid"] in c)
        res[n] = dict(cpu_pct=round((cpu(t[-1]) - cpu(t[0])) / ((t[-1] - t[0]) / 1000) * 100, 2),
                      ws_mb_first_last=[round(sum(float(r["ws_mb"]) for r in ts[t[0]]), 1), round(sum(float(r["ws_mb"]) for r in ts[t[-1]]), 1)],
                      priv_mb_first_last=[round(sum(float(r["priv_mb"]) for r in ts[t[0]]), 1), round(sum(float(r["priv_mb"]) for r in ts[t[-1]]), 1)],
                      minutes=round((t[-1] - t[0]) / 60e3, 1))
p = RAW / f"{name}.macps.csv"
if p.exists():
    def secs(s):
        parts = s.split(":")
        return float(parts[0]) * 60 + float(parts[1]) if len(parts) == 2 else float(parts[0]) * 3600 + float(parts[1]) * 60 + float(parts[2])
    by = defaultdict(list)
    for r in csv.DictReader(p.open()):
        by[(r["comm"], r["pid"])].append((int(r["t"]), secs(r["cpu_time"]), int(r["rss_kb"])))
    for (comm, pid), v in by.items():
        v = [x for x in v if v[0][0] + 60e3 <= x[0] <= v[-1][0] - 60e3] or v
        if len(v) < 2:
            continue
        res[f"mac:{comm}:{pid}"] = dict(cpu_pct=round((v[-1][1] - v[0][1]) / ((v[-1][0] - v[0][0]) / 1000) * 100, 2),
                                        rss_mb_first_last=[round(v[0][2] / 1024, 1), round(v[-1][2] / 1024, 1)], minutes=round((v[-1][0] - v[0][0]) / 60e3, 1))
print(json.dumps(res, indent=1))
