# SPDX-License-Identifier: GPL-3.0-only
"""PC half of one camera E2E run (lebig, over ssh = session 0; standard library only).

    python pcside_cam.py PLAN.json

PLAN = {"dir": run folder, "start_mono": PC time.monotonic() at which the plan's t=0 falls,
        "duration": s, "items": [...pcagent items...], "resources_every": s, "cable": bool,
        "cable_cmd": cmd.exe line (keeps CABLE Output open: demand for a Mac mic stream), "probe": camprobe.exe,
        "kills": [{"at": s}]  (kill the PC helper at plan time `at`: cam_helper_kill)}

1. H8 check: an interactive user must be signed in (the camera reads run in his session); else exit 3.
2. Starts pcagent.py in that session through a one-shot scheduled task (`BeamerCamE2E-agent`, /IT), as
   dev/audio/e2e/pcside.py does for its players: an app that opens "Beamer Camera" must run on the desktop.
3. Optionally keeps CABLE Output open (`cable_cmd`), so a Mac mic routed to the PC streams (on demand).
4. Every second writes {"mono", "unix"} pairs (PC monotonic = QPC <-> wall clock = camprobe's unix µs) to
   pcside.jsonl; every `resources_every` s CPU seconds / working set / private bytes of Beamer, the helper
   (`beamer-audio.exe --control`, not its guard), the Frame Server service host and camprobe.
5. At the end waits for the agent's `done`, kills what is left (camprobe by path, the cable capture),
   deletes the task. Everything is stamped with time.monotonic().
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TASK = "BeamerCamE2E-agent"


def ps(command, timeout=30):
    r = subprocess.run(["powershell", "-NoProfile", "-Command", command], capture_output=True, text=True,
                       errors="replace", timeout=timeout)
    return r.stdout.strip()


def console_user():
    try:
        return ps("(Get-CimInstance Win32_ComputerSystem).UserName", timeout=20) or None
    except subprocess.TimeoutExpired:
        return None


def resources():
    cmd = ("$fs = (Get-CimInstance Win32_Service -Filter \"Name='FrameServer'\").ProcessId; "
           "$h = @(Get-CimInstance Win32_Process -Filter \"Name='beamer-audio.exe'\" | "
           "Where-Object { $_.CommandLine -like '*--control*' } | ForEach-Object { $_.ProcessId }); "
           "$rows = @(); "
           "foreach ($p in @(Get-Process Beamer,beamer-audio,camprobe -EA SilentlyContinue)) { "
           "  $n = $p.ProcessName; if ($n -eq 'beamer-audio') { if ($h -contains $p.Id) { $n = 'helper' } else { $n = 'guard' } }; "
           "  $rows += [pscustomobject]@{name=$n; pid=$p.Id; cpu=$p.CPU; ws=$p.WorkingSet64; priv=$p.PrivateMemorySize64; session=$p.SessionId} }; "
           "if ($fs) { foreach ($p in @(Get-Process -Id $fs -EA SilentlyContinue)) { "
           "  $rows += [pscustomobject]@{name='frameserver'; pid=$p.Id; cpu=$p.CPU; ws=$p.WorkingSet64; priv=$p.PrivateMemorySize64; session=$p.SessionId} } }; "
           "$rows | ConvertTo-Json -Compress")
    try:
        out = ps(cmd, timeout=25)
        data = json.loads(out) if out else []
        return data if isinstance(data, list) else [data]
    except (subprocess.TimeoutExpired, ValueError, OSError) as error:
        return [{"error": str(error)}]


def helper_pid():
    out = ps("(Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'beamer-audio.exe' -and "
             "$_.CommandLine -like '*--control*' }).ProcessId").split()
    return int(out[0]) if out else None


def start_agent(d, plan_path, note):
    pyw = os.path.join(HERE, "venv", "Scripts", "pythonw.exe")
    tr = f'"{pyw}" "{os.path.join(HERE, "pcagent.py")}" "{plan_path}"'
    r1 = subprocess.run(["schtasks", "/Create", "/F", "/TN", TASK, "/SC", "ONCE", "/ST", "00:00", "/TR", tr, "/IT"],
                        capture_output=True, text=True, errors="replace")
    r2 = subprocess.run(["schtasks", "/Run", "/TN", TASK], capture_output=True, text=True, errors="replace")
    note(event="agent_task", create_rc=r1.returncode, run_rc=r2.returncode,
         err=(r1.stderr + r2.stderr).strip()[:300])
    return r1.returncode == 0 and r2.returncode == 0


def kill_by_path(exe_path, note):
    out = ps("Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -eq '%s' } | "
             "ForEach-Object { $_.ProcessId }" % exe_path.replace("'", "''"))
    for pid in out.split():
        subprocess.run(["taskkill", "/T", "/F", "/PID", pid], capture_output=True)
        note(event="killed_leftover", exe=exe_path, pid=int(pid))


def main():
    plan_path = sys.argv[1]
    plan = json.load(open(plan_path, encoding="utf-8"))
    d = plan["dir"]
    os.makedirs(d, exist_ok=True)
    log = open(os.path.join(d, "pcside.jsonl"), "a", buffering=1)

    def note(**kw):
        kw.setdefault("mono", time.monotonic())
        log.write(json.dumps(kw) + "\n")

    user = console_user()
    note(event="start", user=user, plan=plan)
    if not user:
        note(event="no_user")
        print("H8: no interactive user signed in on this PC", flush=True)
        sys.exit(3)
    res = open(os.path.join(d, "resources.jsonl"), "a", buffering=1)
    res.write(json.dumps({"mono": time.monotonic(), "procs": resources()}) + "\n")
    agent_ok = True
    if plan.get("items"):
        agent_ok = start_agent(d, plan_path, note)
    cable = None
    if plan.get("cable") and plan.get("cable_cmd"):
        err = open(os.path.join(d, "cable.err"), "ab")
        cable = subprocess.Popen(["cmd", "/c", plan["cable_cmd"]], stdout=subprocess.DEVNULL, stderr=err, cwd=d)
        note(event="cable_start", pid=cable.pid)
    end = plan["start_mono"] + plan["duration"]
    every = plan.get("resources_every") or 0
    next_res = time.monotonic() + every if every else None
    next_pair = 0.0
    done_path = os.path.join(d, "agent.done")
    kills = sorted(k["at"] for k in plan.get("kills", []))
    victim = None
    while time.monotonic() < end:
        now = time.monotonic()
        if kills and victim is None and now >= plan["start_mono"] + kills[0] - 4.0:
            victim = helper_pid()           # looked up early: the CIM query takes ~1 s
            note(event="helper_pid", pid=victim)
        if kills and now >= plan["start_mono"] + kills[0]:
            kills.pop(0)
            t_before = time.monotonic()
            r = subprocess.run(["taskkill", "/F", "/PID", str(victim)], capture_output=True, text=True,
                               errors="replace") if victim else None
            note(event="kill_helper", pid=victim, mono=t_before, mono_after=time.monotonic(),
                 rc=r.returncode if r else None, out=(r.stdout + r.stderr).strip()[:200] if r else "no pid")
            victim = None
        if now >= next_pair:
            next_pair = now + 1.0
            note(event="pair", unix=time.time())
        if next_res is not None and now >= next_res:
            next_res = now + every
            res.write(json.dumps({"mono": time.monotonic(), "procs": resources()}) + "\n")
        time.sleep(0.05)
    # wait for the agent's last read to end (its items carry their own durations)
    wait_until = time.monotonic() + (60 if agent_ok and plan.get("items") else 0)
    while time.monotonic() < wait_until and not os.path.exists(done_path):
        note(event="pair", unix=time.time())
        time.sleep(1.0)
    note(event="agent_done" if os.path.exists(done_path) else "agent_not_done")
    if cable is not None:
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(cable.pid)], capture_output=True)
        note(event="cable_stop")
    if plan.get("probe"):
        kill_by_path(plan["probe"], note)
    subprocess.run(["schtasks", "/Delete", "/F", "/TN", TASK], capture_output=True)
    res.write(json.dumps({"mono": time.monotonic(), "procs": resources()}) + "\n")
    for _ in range(3):
        note(event="pair", unix=time.time())
        time.sleep(0.2)
    note(event="done")
    print("pcside_cam done", flush=True)


if __name__ == "__main__":
    main()
