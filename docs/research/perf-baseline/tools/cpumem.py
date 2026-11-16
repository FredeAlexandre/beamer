"""CPU (% of one core) and memory of Beamer processes per 5 s sample, labelled by who holds the input.

States, from both logs (each on its own clock; a 5 s sample makes the ~0.04 s offset irrelevant):
  pc_drives_mac  PC "input is on Alexandres-MacBook-Air" .. PC "input returned to this PC"
  mac_drives_pc  Mac "redirecting False -> True" .. Mac "redirecting True -> False"
  local          otherwise (each machine used on its own; Beamer watching, nothing crossing)
usage: cpumem.py <sampler.jsonl> <logdir>
"""
import json, re, sys, statistics as st
from collections import defaultdict
from datetime import datetime
F, L = sys.argv[1], sys.argv[2]
def ts(line): return datetime.strptime(line[:23], "%Y-%m-%d %H:%M:%S,%f").timestamp()
ev, ev_side = [], {}
for side, pat, val in (("pc", r"input is on Alexandres-MacBook-Air", "pc_drives_mac"), ("pc", r"input returned to this PC", "local"),
                       ("mac", r"redirecting False -> True", "mac_drives_pc"), ("mac", r"redirecting True -> False", "local")):
    for l in open(f"{L}/{side}/Beamer.log", errors="replace"):
        if re.search(pat, l): ev.append((ts(l), val)); ev_side[ts(l)] = side
ev.sort()
# A PC "input returned" only ends the PC driving the Mac; it must not end the Mac driving the PC.
clean, cur = [], "local"
for t, v in ev:
    if v == "local" and cur == "mac_drives_pc" and ev_side.get(t) == "pc": continue
    clean.append((t, v)); cur = v
ev = clean
def state(t0, t1):
    s, seen = "local", set()
    for t, v in ev:
        if t <= t0: s = v
        elif t < t1: seen.add(v)
    seen.add(s)
    return s if len(seen) == 1 else "mixed"
recs = [json.loads(l) for l in open(F) if '"k": "mac"' in l or '"k": "pc"' in l]
out = defaultdict(lambda: defaultdict(list))
for machine in ("mac", "pc"):
    rs = sorted((r for r in recs if r["k"] == machine), key=lambda r: r["t"])
    for a, b in zip(rs, rs[1:]):
        dt = b["t"] - a["t"]
        if not 3 < dt < 8: continue
        prev = {p["pid"]: p for p in a["procs"]}
        for p in b["procs"]:
            if p["pid"] not in prev: continue
            name = p["name"] if p["name"] != "Beamer" or machine == "mac" else ("Beamer" if p.get("priv", 0) > 30e6 else "Beamer-launcher")
            cpu = (p["cpu_s"] - prev[p["pid"]]["cpu_s"]) / dt * 100
            mem = p.get("rss_kb", 0) / 1024 if machine == "mac" else p.get("priv", 0) / 2**20
            out[(machine, name)][state(a["t"], b["t"])].append((cpu, mem))
def pct(xs, q): xs = sorted(xs); return xs[min(len(xs) - 1, int(q * len(xs)))]
print("machine process          state           n   cpu% p50  p95   max   mem MB p50 max  (mac: RSS, pc: private bytes)")
for (m, n), by in sorted(out.items()):
    if n in ("VTEncoderXPCService", "Beamer-launcher"): continue
    for s, xs in sorted(by.items()):
        c, mm = [x[0] for x in xs], [x[1] for x in xs]
        print(f"{m:4s}    {n:24s} {s:14s} {len(xs):4d}  {pct(c,.5):6.1f} {pct(c,.95):5.1f} {max(c):5.1f}   {pct(mm,.5):7.0f} {max(mm):5.0f}")
