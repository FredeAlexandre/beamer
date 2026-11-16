"""Baseline numbers from Beamer logs already on disk (both machines).

usage: logstats.py <logdir>   (expects mac/Beamer.log* and pc/Beamer.log*)
"""
import glob, math, re, sys, statistics as st
from collections import defaultdict

root = sys.argv[1]


def lines(side):
    for path in sorted(glob.glob(f"{root}/{side}/Beamer.log*"), reverse=True):
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                yield line.rstrip("\r\n")


def pct(xs, p):
    xs = sorted(xs)
    if not xs:
        return float("nan")
    return xs[min(len(xs) - 1, max(0, math.ceil(len(xs) * p) - 1))]


def summ(xs):
    return f"n={len(xs)} p50={pct(xs,.5):.1f} p95={pct(xs,.95):.1f} p99={pct(xs,.99):.1f} max={max(xs):.1f}" if xs else "n=0"


RT = re.compile(r"^(\S+ \S+) .*round trip to (.+?): (\d+) samples, median ([\d.]+) ms, 95th percentile ([\d.]+) ms, largest ([\d.]+) ms, mean held_us (\d+)")
LT = re.compile(r"^(\S+ \S+) .*link timing (inbound|outbound) (\S+) ping_rtt_ms=(\S+) (.*)")
PAIR = "192.168.1.138"  # the Mac; the PC is .134, .135 is another peer
FIELD = re.compile(r"(\w+)_ms\(count/mean/max\)=(\d+)/([\d.]+)/([\d.]+)")

for side in ("pc", "mac"):
    rts = []
    ping = defaultdict(list)
    stages = defaultdict(lambda: [0, 0.0, 0.0, []])  # count, sum, max, window means
    first = last = None
    for line in lines(side):
        m = RT.match(line)
        if m:
            rts.append((m.group(1), int(m.group(3)), float(m.group(4)), float(m.group(5)), float(m.group(6)), int(m.group(7))))
            continue
        m = LT.match(line)
        if m:
            if PAIR not in m.group(3):
                continue
            first = first or m.group(1)
            last = m.group(1)
            if m.group(4) != "unavailable":
                ping[m.group(2)].append(float(m.group(4)))
            for name, c, mean, mx in FIELD.findall(m.group(5)):
                s = stages[(m.group(2), name)]
                c, mean, mx = int(c), float(mean), float(mx)
                s[0] += c; s[1] += c * mean; s[2] = max(s[2], mx); s[3].append(mx)
    print(f"=== {side} (link timing {first} .. {last})")
    if rts:
        n = sum(r[1] for r in rts)
        medians = [r[2] for r in rts for _ in range(r[1])]  # sample-weighted
        p95s = [r[3] for r in rts]
        print(f"input ACK RTT minus receiver hold, {len(rts)} one-minute windows, {n} samples, {rts[0][0]} .. {rts[-1][0]}, peer {rts[0][1] and rts[-1][1] and ''}")
        print(f"  per-minute medians (sample-weighted): p50={pct(medians,.5):.1f} p95={pct(medians,.95):.1f}")
        print(f"  per-minute p95s: p50={pct(p95s,.5):.1f} p95={pct(p95s,.95):.1f} p99={pct(p95s,.99):.1f}")
        print(f"  per-minute largest: {summ([r[4] for r in rts])}")
        print(f"  mean held_us: {st.mean(r[5] for r in rts):.0f}")
    for d, xs in ping.items():
        print(f"ping RTT ms, {d} link, one per 30 s window: {summ(xs)}")
    for (d, name), (c, tot, mx, maxes) in sorted(stages.items()):
        print(f"  stage {d:8s} {name:20s} count={c:8d} mean={tot/c:.3f} ms  p95-of-window-max={pct(maxes,.95):.3f}  max={mx:.3f}")
