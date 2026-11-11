"""Markdown tables for the report from out/*.summary.json."""
import json, sys
runs = sys.argv[1:]
S = {n: json.load(open(f"out/{n}.summary.json")) for n in runs}
def mode(m):
    if m.get("tcp"): return "TCP (ssh -L)"
    if m.get("pace_bps"): return f"bucket {m['burst_pk']} pk + {m['pace_bps']/1e6:.0f} Mbit/s"
    return "burst" if not m.get("pace") else f"paced {m['pace']}x"
print("| run | rate | keyframe | sending | video pk lost (bursts) | frame net p50/p95/p99/p99.9/max ms | frame total p50/p99 ms | keyframe total p50/p99 ms | frames lost /min | spikes > 45 ms /min | worst |")
print("|---|---|---|---|---|---|---|---|---:|---:|---|")
for n, s in S.items():
    m = s["meta"]
    if "frames" not in s: continue
    v, f, sp = s["video_pk"], s["frames"], s["net_spikes"]
    bursts = ", ".join(f"{k}x{c}" if c > 1 else k for k, c in sorted(v["burst_hist"].items(), key=lambda x: int(x[0]))) or "-"
    print(f"| `{n}` | {m['rate']/1e6:g} Mbit/s | {f['key_kb_mean']:.0f} KB / 2 s | {mode(m)} | {v['lost']} of {v['sent']} ({bursts}) | "
          f"{f['net_ms_p50']:.1f} / {f['net_p95']:.1f} / {f['net_p99']:.1f} / {f['net_p999']:.0f} / {f['net_max']:.0f} | "
          f"{f['delay_ms_p50']:.1f} / {f['p99']:.1f} | {f['key_delay_ms_p50']:.0f} / {f['key_p99']:.0f} | {f['lost_per_min']:.1f} | {sp['gt45ms_per_min']:.1f} | {sp['worst_ms'] if sp['worst_ms'] < 9000 else 'loss'} |")
print()
print("| run | video | sending | audio-like p50 / p95 / p99 / p99.9 / max ms | audio lost | RR (PC->Mac) p50 / p95 / p99 ms | ping RTT min / p50 / p99 ms |")
print("|---|---|---|---|---:|---|---|")
for n, s in S.items():
    m, a, r, c = s["meta"], s["audio"], s["rr"], s["clock"]
    vid = "none" if not m.get("video") else f"{m['rate']/1e6:g} Mbit/s"
    print(f"| `{n}` | {vid} | {mode(m)} | {a['delay_ms_p50']:.1f} / {a['p95']:.1f} / {a['p99']:.1f} / {a['p999']:.0f} / {a['max']:.0f} | {a['lost']} | "
          f"{r['delay_ms_p50']:.1f} / {r['p95']:.1f} / {r['p99']:.1f} | {c['rtt_min_ms']:.1f} / {c['rtt_p50_ms']:.1f} / {c['rtt_p99_ms']:.1f} |")
