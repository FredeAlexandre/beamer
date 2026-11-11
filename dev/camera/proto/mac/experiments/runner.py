#!/usr/bin/env python3
"""Run beamer-camera once and add the CPU of the VideoToolbox XPC services it uses.

VideoToolbox runs the encoder in a VTEncoderXPCService spawned for the client (and decodes in
VTDecoderXPCService), so the tool's own getrusage misses most of the cost. This samples
`ps` every 0.25 s and attributes the CPU-time growth of those services during the run.

    runner.py OUT.jsonl TAG -- run --size 1280x720 ...
Appends one JSON line: the tool's `summary` event + xpc_cpu_pct + the start event's encoder info.
"""
import json, subprocess, sys, time, os

TOOL = os.path.join(os.path.dirname(__file__), "..", "build", "beamer-camera")
SERVICES = ("VTEncoderXPCService", "VTDecoderXPCService")


def cputime(s):
    # [[dd-]hh:]mm:ss.cc
    days = 0
    if "-" in s:
        d, s = s.split("-", 1)
        days = int(d)
    parts = [float(p) for p in s.split(":")]
    sec = 0.0
    for p in parts:
        sec = sec * 60 + p
    return sec + days * 86400


def snapshot():
    out = subprocess.run(["ps", "-A", "-o", "pid=,time=,comm="], capture_output=True, text=True).stdout
    res = {}
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and any(s in parts[2] for s in SERVICES):
            res[int(parts[0])] = (cputime(parts[1]), os.path.basename(parts[2]))
    return res


def main():
    out_path, tag = sys.argv[1], sys.argv[2]
    args = sys.argv[sys.argv.index("--") + 1:]
    before = snapshot()
    peak = dict(before)
    t0 = time.monotonic()
    p = subprocess.Popen([TOOL] + args + ["--tag", tag], stderr=subprocess.PIPE, text=True)
    lines = []
    import threading

    def reader():
        for line in p.stderr:
            lines.append(line)
    th = threading.Thread(target=reader, daemon=True)
    th.start()
    while p.poll() is None:
        for pid, v in snapshot().items():
            if pid not in peak or v[0] >= peak[pid][0]:
                peak[pid] = v
        time.sleep(0.25)
    th.join()
    wall = time.monotonic() - t0
    xpc = {}
    for pid, (t, name) in peak.items():
        d = t - before.get(pid, (0.0, name))[0]
        if d > 0:
            xpc[name] = xpc.get(name, 0.0) + d
    events = []
    for l in lines:
        l = l.strip()
        if l.startswith("{"):
            try:
                events.append(json.loads(l))
            except json.JSONDecodeError:
                pass
        elif l:
            sys.stderr.write(l + "\n")
    summary = next((e for e in reversed(events) if e.get("ev") == "summary"), {"ev": "no_summary"})
    start = next((e for e in events if e.get("ev") == "start"), {})
    summary["args"] = " ".join(args)
    summary["xpc_cpu_pct"] = {k: round(v / wall * 100, 1) for k, v in xpc.items()}
    summary["start"] = start
    summary["other_events"] = [e for e in events if e.get("ev") not in ("summary", "start", "enc_stats", "cam_stats")]
    with open(out_path, "a") as f:
        f.write(json.dumps(summary) + "\n")
    if os.environ.get("RUNNER_EVENTS"):  # keep every event of this run too
        with open(os.environ["RUNNER_EVENTS"], "w") as f:
            f.writelines(json.dumps(e) + "\n" for e in events)
    print(json.dumps({k: summary.get(k) for k in ("tag", "encoder_id", "kbps_mean", "kbps_1s", "enc_ms", "key_bytes", "p_bytes",
                                                     "cpu_pct_excl_synth", "xpc_cpu_pct", "dropped", "psnr_y")}))
    return p.returncode


if __name__ == "__main__":
    sys.exit(main())
