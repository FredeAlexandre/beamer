"""Clipboard 'cross, then ready to paste' latency.

A receiver-side clipboard change within 2 s after a crossing toward the receiver counts as the
transfer; the sender-side crossing time is moved onto the receiver's clock with the PC-Mac offset
measured (lowest-RTT pings within +-60 s) by sampler.py. Changes the receiver made itself (copies
made there before crossing away) are not preceded by such a crossing and are ignored.
usage: clipmatch.py <scratch> <logdir>
"""
import json, re, sys
from datetime import datetime
S, L = sys.argv[1], sys.argv[2]
def ts(line): return datetime.strptime(line[:23], "%Y-%m-%d %H:%M:%S,%f").timestamp()
def log(side, pat): return [ts(l) for l in open(f"{L}/{side}/Beamer.log", errors="replace") if re.search(pat, l)]
clock = [json.loads(l) for l in open(f"{S}/sampler-nov16.jsonl") if '"k": "clock"' in l]
def off(t):  # PC - Mac, seconds
    near = sorted((r for r in clock if abs(r["t0"] - t) < 60), key=lambda r: r["rtt"])[:5]
    return sorted(r["off"] for r in near)[len(near) // 2]
def changes(f): return [json.loads(l)["t"] for l in open(f"{S}/{f}") if '"change"' in l]
pc_to_mac = [t for t in log("pc", r"input is on Alexandres-MacBook-Air") if t > clock[0]["t0"] + 30]           # PC clock
mac_to_pc = [t for t in log("mac", r"pushed through the zone .* asking lebig") if t > clock[0]["t0"] + 30]     # Mac clock
warn = log("pc", r"clipboard image is invalid or too large")
print("PC->Mac (PC crossing, Mac pasteboard change), ms:")
for t in pc_to_mac:
    tm = t - off(t)
    c = [x for x in changes("clip-mac-nov16.jsonl") if 0 <= x - tm < 2]
    print(f"  crossing {datetime.fromtimestamp(tm):%H:%M:%S.%f}"[:-3], f"-> {(c[0]-tm)*1000:.0f}" if c else "-> no change")
print("Mac->PC (Mac crossing, PC clipboard sequence change), ms:")
for t in mac_to_pc:
    tp = t + off(t)
    c = [x for x in changes("clip-pc-nov16.jsonl") if 0 <= x - tp < 2]
    print(f"  crossing {datetime.fromtimestamp(t):%H:%M:%S.%f}"[:-3], f"-> {(c[0]-tp)*1000:.0f} (last of burst {(c[-1]-tp)*1000:.0f})" if c else "-> no change")
print("PC 'image invalid or too large' warnings:", [f"{datetime.fromtimestamp(w):%H:%M:%S}" for w in warn])
