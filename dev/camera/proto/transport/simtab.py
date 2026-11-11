"""Pretty-print sim.py output lines (stdin) as a markdown table."""
import json, sys
print("| run | policy | D ms | frozen ms/min | freezes/min | > 100 ms /min | longest ms | degraded ms/min | IDR/min | kbit/s | rtx pk/min |")
print("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
for l in sys.stdin:
    if " {" not in l:
        continue
    name, js = l.split(" ", 1)
    r = json.loads(js)
    r.setdefault("run", name)
    print(f"| {r.get('run', '')} | {r['policy']} | {r['D_ms']}{' (mean '+str(r['D_mean_ms'])+')' if 'D_mean_ms' in r else ''} | {r['frozen_ms_per_min']} | {r['freezes_per_min']} | {r['freezes_gt100ms_per_min']} | {r['longest_freeze_ms']} | {r['degraded_ms_per_min']} | {r['idr_per_min']} | {r['kbps']} | {r['rtx_pk_per_min']} |")
