# SPDX-License-Identifier: GPL-3.0-only
"""Markdown table of E2E runs from their result.json (for dev/audio/reports/wp10.md and STATUS.md).

    python3 dev/audio/e2e/table.py RUN [RUN ...]       (run names under runs/ or paths)
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

RUNS = Path(__file__).resolve().parent / "runs"


def cell(run):
    d = Path(run) if Path(run).exists() else RUNS / run
    r = json.load(open(d / "result.json"))
    lat = "; ".join(f"{x['src'].split('/')[1]}->{x['sink']}: {x['p50_ms']}/{x['p95_ms']} ({x['matched']}/{x['n_src']})"
                    for x in r["latency"] if not x["src"].endswith("inj"))
    gl = "; ".join(f"{k}: {v.get('events_per_min')} ({v.get('episodes_per_min')} ep)"
                   for k, v in r["glitches"].items() if v and v.get("seconds") and k.split(":")[0] in
                   {x["sink"] for x in r["latency"] if not x["src"].endswith("inj")})
    jb = []
    for side in ("mac", "pc"):
        for s in r["helpers"][side]["streams"].values():
            if s["dir"] == "recv" and s["intervals"] > 10:
                jb.append(f"{side} {s['kind']} {s['jb_ms_mean']}/{s['target_ms_mean']} ms, und {s['underrun']}, "
                          f"exp {s['expand']}, ppm {s.get('ppm_last_60s_mean')}")
    cpu = "; ".join(f"{side} helper {r['helpers'][side]['helper_cpu_pct_mean']}" for side in ("mac", "pc"))
    res = r.get("resources", {})
    app = "; ".join(f"{side} {k.split(':')[0]} {v.get('cpu_pct')}" for side in ("mac", "pc")
                    for k, v in res.get(side, {}).items() if k.startswith("Beamer:") and v.get("mem_mb_first", 0) > 10)
    return f"| {d.name} | {lat} | {gl} | {'; '.join(jb)} | {cpu}; {app} |"


def main():
    print("| run | latency p50/p95 ms (matched) | glitches/min (episodes) | JB mean/target, underruns, ppm | CPU % of a core |")
    print("|---|---|---|---|---|")
    for run in sys.argv[1:]:
        print(cell(run))


if __name__ == "__main__":
    main()
