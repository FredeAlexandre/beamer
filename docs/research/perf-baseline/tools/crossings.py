"""Crossing latency from both machines' Beamer logs (each stamped with its own wall clock).

PC->Mac: PC "input is on <Mac>, at its right edge"  ->  Mac "lebig's pointer arrived"      d1 = g - o
Mac->PC: Mac "pushed through the zone ... asking"     ->  PC "input returned to this PC (switch)"  d2 = r + o
o = PC clock - Mac clock. d1 + d2 = g + r does not depend on o.
usage: crossings.py <logdir> <offset_ms measured now>
"""
import glob, math, re, sys
from datetime import datetime
root, off = sys.argv[1], float(sys.argv[2])
def ts(line): return datetime.strptime(line[:23], "%Y-%m-%d %H:%M:%S,%f").timestamp() * 1000
def grab(side, pat):
    out = []
    for p in glob.glob(f"{root}/{side}/Beamer.log*"):
        for l in open(p, encoding="utf-8", errors="replace"):
            if re.search(pat, l): out.append(ts(l))
    return sorted(out)
pc_go = grab("pc", r"input is on Alexandres-MacBook-Air, at its right edge")
mac_arr = grab("mac", r"lebig's pointer arrived")
mac_back = grab("mac", r"pushed through the zone at this machine's right edge; asking lebig")
pc_ret = grab("pc", r"input returned to this PC \(switch\)")
def pair(a, b, lo=-2000, hi=2000):
    res = []
    for t in a:
        c = [x - t for x in b if lo < x - t < hi]
        if c: res.append(min(c, key=abs))
    return res
def pct(xs, p):
    xs = sorted(xs); return xs[min(len(xs)-1, max(0, math.ceil(len(xs)*p)-1))]
def s(xs): return f"n={len(xs)} p50={pct(xs,.5):.0f} p95={pct(xs,.95):.0f} p99={pct(xs,.99):.0f} min={min(xs):.0f} max={max(xs):.0f}"
d1, d2 = pair(pc_go, mac_arr), pair(mac_back, pc_ret)
print("raw d1 PC->Mac (mac - pc):", s(d1)); print("raw d2 Mac->PC (pc - mac):", s(d2))
print(f"with offset o={off:.1f} ms: PC->Mac g = d1 + o:", s([x + off for x in d1]))
print(f"with offset o={off:.1f} ms: Mac->PC r = d2 - o:", s([x - off for x in d2]))
print("offset-free: median(d1)+median(d2) = g + r =", f"{pct(d1,.5)+pct(d2,.5):.0f} ms")
print("first/last crossing:", datetime.fromtimestamp(min(pc_go)/1000), datetime.fromtimestamp(max(pc_go)/1000))

# Offset-free per pair: each PC->Mac crossing with the next Mac->PC return within 10 min,
# assuming the PC clock moves less than a few ms in that interval.
go = [(t, min((x - t for x in mac_arr if -2000 < x - t < 2000), key=abs)) for t in pc_go]
back = [(t, min((x - t for x in pc_ret if -2000 < x - t < 2000), key=abs)) for t in mac_back]
sums, gaps = [], []
for t, a in go:
    nxt = [(bt, b) for bt, b in back if 0 < bt - t < 600_000]
    if nxt:
        bt, b = min(nxt)
        sums.append(a + b); gaps.append((bt - t) / 1000)
print("per-pair g + r (offset-free, two crossings):", s(sums))
print("so one crossing ~ (g + r) / 2:", s([x / 2 for x in sums]))
print("seconds between the pair's two crossings: median", sorted(gaps)[len(gaps)//2])
