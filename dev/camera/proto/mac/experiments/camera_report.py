#!/usr/bin/env python3
"""Summarise camera_suite.sh output (prints markdown tables)."""
import glob, json, os, statistics as st, sys

O = sys.argv[1]


def events(path):
    try:
        return [json.loads(l) for l in open(path) if l.startswith("{")]
    except FileNotFoundError:
        return []


def first(evs, name):
    return next((e for e in evs if e.get("ev") == name), None)


def med(v):
    return round(st.median(v), 1) if v else None


print("## Cold start (separate process, startRunning -> first frame)\n")
print("| size | startRunning call ms (median, max) | first frame after call ms (median, max) | n |")
print("|---|---|---|---|")
for sz in ("1280x720", "1920x1080"):
    sr, ff = [], []
    for p in sorted(glob.glob(f"{O}/cold_{sz}_*.jsonl")):
        ev = events(p)
        s, f = first(ev, "started"), first(ev, "first_frame")
        if s: sr.append(s["start_running_ms"])
        if f: ff.append(f["since_start_call_ms"])
    print(f"| {sz} | {med(sr)}, {max(sr) if sr else None} | {med(ff)}, {max(ff) if ff else None} | {len(ff)} |")

ev = events(f"{O}/warm.jsonl")
ff = [e["since_start_call_ms"] for e in ev if e.get("ev") == "first_frame"]
sp = [e["stop_running_ms"] for e in ev if e.get("ev") == "stopped"]
print(f"\nWarm restarts in one process: first frame after startRunning call {ff} ms; stopRunning {sp} ms\n")

print("## Capture runs\n")
print("| run | fps per second (from PTS) | PTS->delegate ms p50/p95/max | interval ms p50/p95/max | cap->encoded ms p50/p95 | enc ms p50/p95 | kbps | CPU own % | VT XPC % |")
print("|---|---|---|---|---|---|---|---|---|")
runs = {}
for l in open(f"{O}/runs.jsonl") if os.path.exists(f"{O}/runs.jsonl") else []:
    s = json.loads(l)
    runs[s.get("tag")] = s
for tag in ("cam720", "cam720_pin", "cam1080"):
    s = runs.get(tag)
    if not s:
        continue
    ev = events(f"{O}/{tag}.jsonl")
    fps = [e["fps_from_pts"] for e in ev if e.get("ev") == "cam_stats"]
    lat, iv = s.get("cam_pts_to_arrival_ms", {}), s.get("cam_interval_ms", {})
    c2o, enc = s.get("cap_to_out_ms", {}), s.get("enc_ms", {})
    r = lambda x: None if x is None else round(x, 1)
    print(f"| {tag} | {fps} | {r(lat.get('p50'))}/{r(lat.get('p95'))}/{r(lat.get('max'))} | {r(iv.get('p50'))}/{r(iv.get('p95'))}/{r(iv.get('max'))} "
          f"| {c2o.get('p50')}/{c2o.get('p95')} | {enc.get('p50')}/{enc.get('p95')} | {s.get('kbps_mean')} | {s.get('cpu_pct_total')} | {s.get('xpc_cpu_pct')} |")

print("\n## Sharing\n")
ev = events(f"{O}/share_main.jsonl")
for e in ev:
    if e.get("ev") == "cam_stats":
        print(f"main t={e['t']}s frames={e['frames']} fps={e['fps_from_pts']} size={e['size']} active={e['active_format']} in_use_by_other={e['in_use_by_another_app']}")
    elif e.get("ev") not in ("enc_stats", "start", "summary"):
        print("main event:", json.dumps(e)[:300])
ev2 = events(f"{O}/share_second.jsonl")
f2 = first(ev2, "first_frame")
print("second session first frame:", f2)
print("second session fps:", [e["fps_from_pts"] for e in ev2 if e.get("ev") == "cam_stats"], [e["size"] for e in ev2 if e.get("ev") == "cam_stats"])
if os.path.exists(f"{O}/share_ffmpeg.txt"):
    tail = open(f"{O}/share_ffmpeg.txt").read().strip().splitlines()[-4:]
    print("ffmpeg tail:", " | ".join(tail))

print("\n## Effects\n")
ev = events(f"{O}/effects.jsonl")
print("before:", json.dumps((first(ev, "effects_before") or {})))
s = first(ev, "start") or {}
print("center stage app-control error:", repr(s.get("center_stage_app_control_error")))
print("while capturing:", [e.get("effects") for e in ev if e.get("ev") == "cam_stats"][:2])
