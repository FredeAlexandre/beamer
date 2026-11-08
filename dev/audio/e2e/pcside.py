# SPDX-License-Identifier: GPL-3.0-only
"""PC half of one E2E run (runs on lebig over ssh; Python 3.13 venv with numpy, see run.py).

    python pcside.py PLAN.json

PLAN = {"dir": run folder, "duration": s, "procs": [{"name", "cmd" | "session1", "delay"}],
        "poll": {"period": s, "match": ["G733", ...]} | null, "resources_every": s | 0}

Starts each `cmd` (a cmd.exe line, so pipes work) after its delay with stderr to <name>.err; a
`session1` {"gen": argv, "play": argv} pipe runs in the signed-in session instead (pcplay.py through a
one-shot scheduled task `BeamerE2E-play`, deleted again), because Windows process loopback, which
Beamer's helper uses for "PC sound", only hears its own session. Then it polls
the endpoints' mute/volume with `proto-audio.exe list --json` into endpoints.jsonl, samples Beamer's
and the helper's CPU time and memory into resources.jsonl, waits for the processes it started (they
carry their own --seconds), then kills the leftovers of its own process trees. Everything it writes
is stamped with time.monotonic() (QueryPerformanceCounter), the clock clock.py relates to the Mac.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROTO = os.path.join(HERE, "proto-audio.exe")
TASK = "BeamerE2E-play"


def start_session1(d, name, spec, note):
    sp = os.path.join(d, name + ".s1.json")
    pids = os.path.join(d, name + ".pids")
    with open(sp, "w", encoding="ascii") as f:
        json.dump({"gen": spec["gen"], "play": spec["play"], "err": os.path.join(d, name + ".err"), "pids": pids}, f)
    pyw = os.path.join(HERE, "venv", "Scripts", "pythonw.exe")
    tr = f'"{pyw}" "{os.path.join(HERE, "pcplay.py")}" "{sp}"'
    r1 = subprocess.run(["schtasks", "/Create", "/F", "/TN", TASK, "/SC", "ONCE", "/ST", "00:00", "/TR", tr, "/IT"],
                        capture_output=True, text=True, errors="replace")
    r2 = subprocess.run(["schtasks", "/Run", "/TN", TASK], capture_output=True, text=True, errors="replace")
    note(event="session1", name=name, create_rc=r1.returncode, run_rc=r2.returncode)
    return pids


def stop_session1(pids_path, note):
    try:
        pids = json.load(open(pids_path))
    except (OSError, ValueError):
        pids = {}
    for pid in pids.values():
        r = subprocess.run(["taskkill", "/T", "/F", "/PID", str(pid)], capture_output=True)
        if r.returncode == 0:
            note(event="killed_session1", pid=pid)
    subprocess.run(["schtasks", "/Delete", "/F", "/TN", TASK], capture_output=True)


def resources():
    """Beamer.exe / beamer-audio.exe: pid, CPU seconds, working set and private bytes."""
    ps = ("Get-Process Beamer,beamer-audio -EA SilentlyContinue | ForEach-Object { "
          "[pscustomobject]@{name=$_.ProcessName; pid=$_.Id; cpu=$_.CPU; ws=$_.WorkingSet64; "
          "priv=$_.PrivateMemorySize64; session=$_.SessionId} } | ConvertTo-Json -Compress")
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True,
                             timeout=20).stdout.strip()
        data = json.loads(out) if out else []
        return data if isinstance(data, list) else [data]
    except (subprocess.TimeoutExpired, ValueError, OSError) as error:
        return [{"error": str(error)}]


def endpoints(match):
    try:
        out = subprocess.run([PROTO, "list", "--json"], capture_output=True, text=True, timeout=5).stdout
        rows = json.loads(out)
    except (subprocess.TimeoutExpired, ValueError, OSError):
        return None
    return {e["name"]: {"mute": e.get("mute"), "volume": e.get("volume"), "default": e.get("default_console")}
            for e in rows if e.get("flow") == "render" and any(m in e.get("name", "") for m in match)}


def main():
    plan = json.load(open(sys.argv[1], encoding="utf-8"))
    d = plan["dir"]
    os.makedirs(d, exist_ok=True)
    log = open(os.path.join(d, "pcside.jsonl"), "a", buffering=1)

    def note(**kw):
        kw.setdefault("t", time.monotonic())
        log.write(json.dumps(kw) + "\n")
    t0 = time.monotonic()
    note(event="start", plan=plan)
    res = open(os.path.join(d, "resources.jsonl"), "a", buffering=1)
    res.write(json.dumps({"t": time.monotonic(), "procs": resources()}) + "\n")
    pending = sorted(plan.get("procs", []), key=lambda p: p.get("delay", 0))
    running = []
    session1 = []
    poll = plan.get("poll")
    epf = open(os.path.join(d, "endpoints.jsonl"), "a", buffering=1) if poll else None
    next_poll = t0
    every = plan.get("resources_every") or 0
    next_res = t0 + every if every else None
    end = t0 + plan["duration"]
    while time.monotonic() < end or any(p.poll() is None for _, p in running):
        now = time.monotonic()
        while pending and now - t0 >= pending[0].get("delay", 0):
            spec = pending.pop(0)
            if spec.get("session1"):
                session1.append(start_session1(d, spec["name"], spec["session1"], note))
                continue
            err = open(os.path.join(d, spec["name"] + ".err"), "ab")
            p = subprocess.Popen(["cmd", "/c", spec["cmd"]], stdout=subprocess.DEVNULL, stderr=err, cwd=d)
            running.append((spec["name"], p))
            note(event="spawn", name=spec["name"], pid=p.pid, cmd=spec["cmd"])
        if epf and now >= next_poll and now < end:
            next_poll = now + poll.get("period", 0.5)
            state = endpoints(poll.get("match", ["G733"]))
            epf.write(json.dumps({"t": now, "t_done": time.monotonic(), "endpoints": state}) + "\n")
        if next_res is not None and now >= next_res and now < end:
            next_res = now + every
            res.write(json.dumps({"t": now, "procs": resources()}) + "\n")
        if now > end + 30:
            break
        time.sleep(0.02)
    for pids_path in session1:
        stop_session1(pids_path, note)
    for name, p in running:
        if p.poll() is None:
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(p.pid)], capture_output=True)
            note(event="killed", name=name, pid=p.pid)
        else:
            note(event="exited", name=name, rc=p.returncode)
    res.write(json.dumps({"t": time.monotonic(), "procs": resources()}) + "\n")
    if epf:
        epf.write(json.dumps({"t": time.monotonic(), "endpoints": endpoints(poll.get("match", ["G733"]))}) + "\n")
    note(event="done")
    print("pcside done", flush=True)


if __name__ == "__main__":
    main()
