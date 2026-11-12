# SPDX-License-Identifier: GPL-3.0-only
"""cam_lipsync (camera PHASE2 §4.9 "Measurement", §10.5; CW10): the A/V offset an app on the PC sees, Mac mic
(helper test source A, `sr:true`) + Mac synthetic camera (C1 code strip), with the PC's virtual mic in shared
and in low-latency (exclusive) mode. Pass: offset within -20..+40 ms (audio later = positive) in both modes.

    uv run --python 3.13 --with numpy dev/camera/e2e/cam_lipsync.py setup       # PC folder, tools, venv
    uv run --python 3.13 --with numpy dev/camera/e2e/cam_lipsync.py run [--modes shared,lowlat] [--seconds 300]
    uv run --python 3.13 --with numpy dev/camera/e2e/cam_lipsync.py analyze dev/camera/e2e/runs/cam_lipsync/<name>
    uv run --python 3.13 --with numpy dev/camera/e2e/cam_lipsync.py selftest    # the analysis on synthetic data

Self-contained (CW11 owns the rest of dev/camera/e2e; nothing of it is imported). Imports dev/audio/e2e
(beamer.py: dev command files; clock.py: the ssh ping clock link) and dev/audio/proto/scenarios (sig.py:
signal A, matched filter, clock fit; tap.py: the PC recorder), never edits them. One file plays every role:
on the PC it runs as `cam_lipsync.py pcside PLAN` (over ssh, session 0: the CABLE Output recorder, every record
stamped with time.monotonic() = QPC) and `cam_lipsync.py agent PLAN` (in Alex's signed-in session through a
one-shot scheduled task, as dev/audio/e2e/pcside.py does for its players: `camprobe read --strip c1` opens
"Beamer Camera" as an app does; its per-frame times are the PC wall clock, related to QPC by pairs sampled
beside it).

Method (§4.9): both media carry their own Mac capture times. Audio: marker k of signal A is captured at
test_t0_ns + marker_index(k) / 48 kHz (helper stats; the time-map pairs logged with `log_time_map` are checked
against it). Video: each picture's code strip carries its Mac capture time (100 us units). On the PC, the time
an app receives each marker (CABLE Output, matched filter) and each picture (camprobe). Offset per marker =
(audio arrival - capture) - (video arrival - capture) of the pictures captured within +-250 ms of the marker:
the Mac/PC clock difference cancels; clock.py only pairs each marker with its arrival.

Preconditions: both Beamer apps deployed from a build with CW10 (the PC's video stats carry `sync_audio_ms`) in
dev mode (`audio-dev.enabled`) with `--dev` helpers (`camera_synth`); Alex signed in on lebig (H8; otherwise the
run stops and says so); nobody else using the camera/audio lab (locks below). Restores the PC's low-latency
switch, the routes and the test sources afterwards. Output: runs/cam_lipsync/<name>/{shared,lowlat}/ and
runs/cam_lipsync/<name>/result.json.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
AUDIO_E2E = REPO / "dev" / "audio" / "e2e"
SCEN = REPO / "dev" / "audio" / "proto" / "scenarios"
RUNS = HERE / "runs" / "cam_lipsync"

PC_HOME = os.environ.get("BEAMER_LIPSYNC_PC_HOME", r"C:\Users\Alex\camera-cw10")
PC_PY = PC_HOME + r"\venv\Scripts\python.exe"
PC_PYW = PC_HOME + r"\venv\Scripts\pythonw.exe"
PC_CLONE = r"C:\Users\Alex\Developer\beamer"
PC_PROBE_SRC = PC_CLONE + r"\native\audio\build\win\camprobe.exe"
PC_PROTO_SRCS = (r"C:\Users\Alex\audio-wp10\proto-audio.exe", r"C:\Users\Alex\audio-proto\scenarios\beamer-audio.exe")
# CABLE Output's endpoint id on lebig (dev/audio/e2e/run.py CABLE_OUT)
CABLE_OUT = "{0.0.1.00000000}.{ddc7e301-3260-4ed3-8389-0938254820e0}"
TASK = "BeamerLipsyncAgent"
LOCKS = (Path.home() / "Developer" / ".beamer-camera-lab.lock",)
AUDIO_LOCK = Path.home() / "Developer" / ".beamer-audio-lab.lock"
DEPLOY_LOCK = Path.home() / "Developer" / ".beamer-deploy.lock"

PASS_LO, PASS_HI = -20.0, 40.0   # §10.5: audio later = positive
LEAD_S = 6.0                     # PC recorders open this long after the run start (routes set at 0)
SETTLE_S = 25.0                  # excluded from the stats: on-demand start, 2 s warm-up, hold ramp at 10 ms/s
STRIP_WRAP_S = (1 << 32) * 1e-4  # the code strip's 32-bit 100 us time
MODES = {"shared": False, "lowlat": True}  # PC `set_vmic_exclusive`


class Stop(Exception):
    pass


# =========================================================================== PC side (stdlib only)

def _pc_note(log, **kw):
    kw.setdefault("t", time.monotonic())
    kw.setdefault("unix", time.time())
    log.write(json.dumps(kw) + "\n")


def pc_side(plan_path):
    """Session 0 over ssh: CABLE Output recorder + the agent in Alex's session; waits for both."""
    plan = json.load(open(plan_path, encoding="utf-8"))
    d = plan["dir"]
    os.makedirs(d, exist_ok=True)
    log = open(os.path.join(d, "pcside.jsonl"), "a", buffering=1)
    t0 = time.monotonic()
    _pc_note(log, event="start", plan=plan)
    rec = None
    agent_started = False
    seconds = plan["seconds"]
    while time.monotonic() - t0 < plan["lead"]:
        time.sleep(0.02)
    # the agent first (camera opens: the video stream starts on demand), then the recorder (CABLE Output
    # opened: the mic stream starts on demand)
    tr = f'"{PC_PYW}" "{os.path.join(PC_HOME, "cam_lipsync.py")}" agent "{plan_path}"'
    r1 = subprocess.run(["schtasks", "/Create", "/F", "/TN", TASK, "/SC", "ONCE", "/ST", "00:00", "/TR", tr, "/IT"],
                        capture_output=True, text=True, errors="replace")
    r2 = subprocess.run(["schtasks", "/Run", "/TN", TASK], capture_output=True, text=True, errors="replace")
    agent_started = r1.returncode == 0 and r2.returncode == 0
    _pc_note(log, event="agent_task", create_rc=r1.returncode, run_rc=r2.returncode,
             err=(r1.stderr + r2.stderr).strip()[:300])
    err = open(os.path.join(d, "cable.err"), "ab")
    cmd = (f'"{PC_PY}" "{PC_HOME}\\tap.py" rec --out "{d}\\cable" --channels 1 --seconds {seconds} -- '
           f'"{PC_HOME}\\proto-audio.exe" capture-mic --device {CABLE_OUT} --channels 1')
    rec = subprocess.Popen(["cmd", "/c", cmd], stdout=subprocess.DEVNULL, stderr=err, cwd=d)
    _pc_note(log, event="spawn", name="cable", pid=rec.pid, cmd=cmd)
    end = time.monotonic() + seconds + 60
    done = os.path.join(d, "agent.done")
    while time.monotonic() < end and (rec.poll() is None or (agent_started and not os.path.exists(done))):
        time.sleep(0.2)
    if rec.poll() is None:
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(rec.pid)], capture_output=True)
        _pc_note(log, event="killed", name="cable")
    else:
        _pc_note(log, event="exited", name="cable", rc=rec.returncode)
    _pc_note(log, event="agent", done=os.path.exists(done))
    subprocess.run(["schtasks", "/Delete", "/F", "/TN", TASK], capture_output=True)
    _pc_note(log, event="done")
    print("pcside done", flush=True)


def pc_agent(plan_path):
    """Alex's session: camprobe reads Beamer Camera; (QPC, wall) pairs sampled beside it every 250 ms."""
    plan = json.load(open(plan_path, encoding="utf-8"))
    d = plan["dir"]
    log = open(os.path.join(d, "agent.jsonl"), "a", buffering=1)
    pairs = open(os.path.join(d, "clockpairs.jsonl"), "a", buffering=1)
    stop = threading.Event()

    def sample():
        while not stop.is_set():
            a = time.monotonic()
            u = time.time()
            b = time.monotonic()
            pairs.write(json.dumps({"mono": (a + b) / 2, "unix": u, "spread": b - a}) + "\n")
            stop.wait(0.25)
    th = threading.Thread(target=sample, daemon=True)
    th.start()
    try:
        _pc_note(log, event="agent_start", session=os.environ.get("SESSIONNAME"), user=os.environ.get("USERNAME"))
        args = [os.path.join(PC_HOME, "camprobe.exe"), "read", "--strip", "c1", "--events", "--dump",
                os.path.join(d, "cam.dump"), "--seconds", str(plan["seconds"]), "--size", plan.get("size", "1280x720"),
                "--tag", "lip"]
        with open(os.path.join(d, "cam.out"), "w", encoding="utf-8") as out, \
                open(os.path.join(d, "cam.err"), "w", encoding="utf-8") as err:
            _pc_note(log, event="spawn", args=args)
            rc = subprocess.run(args, stdout=out, stderr=err, timeout=plan["seconds"] + 60,
                                creationflags=0x08000000).returncode
            _pc_note(log, event="exit", rc=rc)
    except Exception as error:  # noqa: BLE001  (agent.done must be written)
        _pc_note(log, event="error", error=repr(error))
    finally:
        stop.set()
        th.join(timeout=2)
        _pc_note(log, event="agent_done")
        open(os.path.join(d, "agent.done"), "w").close()


# =========================================================================== Mac side: driving

def _mac_imports():
    os.environ["BEAMER_E2E_PC_HOME"] = PC_HOME  # beamer.Pc stages its command file in our PC folder
    sys.path.insert(0, str(AUDIO_E2E))
    import beamer  # noqa: E402
    from clock import ClockLink  # noqa: E402
    return beamer, ClockLink


def wait_for(get, pred, timeout, period=0.5, what="condition"):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        v = get()
        if v is not None and pred(v):
            return v
        time.sleep(period)
    raise Stop(f"timed out after {timeout:.0f} s waiting for {what}")


def setup():
    beamer, _ = _mac_imports()
    beamer.ssh(f"New-Item -ItemType Directory -Force {PC_HOME}\\runs | Out-Null")
    for f in (SCEN / "tap.py", SCEN / "sig.py", AUDIO_E2E / "clock.py", Path(__file__)):
        beamer.scp_to(f, PC_HOME + "\\" + f.name)
    srcs = " ".join(f"'{p}'" for p in PC_PROTO_SRCS)
    out = beamer.ssh(
        f"$ErrorActionPreference='Stop'; "
        f"if (-not (Test-Path {PC_HOME}\\proto-audio.exe)) {{ foreach ($s in @({srcs})) {{ if (Test-Path $s) "
        f"{{ Copy-Item $s {PC_HOME}\\proto-audio.exe; break }} }} }}; "
        f"if (Test-Path '{PC_PROBE_SRC}') {{ Copy-Item -Force '{PC_PROBE_SRC}' {PC_HOME}\\camprobe.exe }}; "
        f"if (-not (Test-Path {PC_PY})) {{ Set-Location {PC_HOME}; uv venv -q --python 3.13 venv; "
        f"uv pip install -q --python venv\\Scripts\\python.exe numpy }}; "
        f"'proto=' + (Test-Path {PC_HOME}\\proto-audio.exe) + ' probe=' + (Test-Path {PC_HOME}\\camprobe.exe) + "
        f"' py=' + (Test-Path {PC_PY})", timeout=600)
    print(out.strip())
    if "False" in out:
        raise Stop("setup incomplete: proto-audio.exe comes from the audio harness (dev/audio/e2e/run.py setup), "
                   "camprobe.exe from the PC build (dev/deploy.sh pc builds native\\audio\\build\\win\\camprobe.exe)")
    print("setup done")


def take_locks(ignore_audio_lock):
    if DEPLOY_LOCK.exists():
        raise Stop(f"{DEPLOY_LOCK} exists: a deploy is running")
    if AUDIO_LOCK.exists() and not ignore_audio_lock:
        raise Stop(f"{AUDIO_LOCK} is held: an audio E2E run may be going on (--ignore-audio-lock if it is stale "
                   f"or yours)")
    taken = []
    for lk in LOCKS:
        try:
            lk.mkdir()
        except FileExistsError:
            for t in taken:
                release([t])
            owner = (lk / "owner").read_text().strip() if (lk / "owner").exists() else "?"
            raise Stop(f"{lk} is held ({owner}): another camera E2E run is going on")
        (lk / "owner").write_text(f"cam_lipsync pid {os.getpid()} since {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
        taken.append(lk)
    return taken


def release(locks):
    for lk in locks:
        try:
            (lk / "owner").unlink()
            lk.rmdir()
        except OSError:
            pass


def alex_signed_in(beamer):
    out = beamer.ssh("(Get-CimInstance Win32_ComputerSystem).UserName", check=False).strip()
    return out or None


def run_mode(beamer, ClockLink, mode, seconds, run_dir, notes):
    """One session: routes on, PC recorder + camera read for `seconds`, routes off. Writes run_dir/*."""
    mac, pc = beamer.Mac(), beamer.Pc()
    run_dir.mkdir(parents=True, exist_ok=True)

    def note(**kw):
        kw.setdefault("t", time.monotonic())
        notes.append(kw)
        print(f"  [{mode}] {kw.get('event')}: " + ", ".join(f"{k}={v}" for k, v in kw.items() if k not in ("t", "event")))
    mst, pst = mac.status(), pc.status()
    if not mst or not pst:
        raise Stop("a dev status file is missing (Beamer in dev mode on both machines?)")
    mac_id, pc_id = mst["snapshot"]["own"], pst["snapshot"]["own"]
    mac_mic = beamer.device(mst, mac_id, "inputs", "BuiltInMicrophoneDevice")
    # 1. the PC's virtual mic mode (restarts the PC helper when it changes)
    want_ex = MODES[mode]
    if bool(pst["snapshot"].get("vmic_exclusive")) != want_ex:
        pc.command({"set_vmic_exclusive": want_ex})
        note(event="vmic_exclusive", on=want_ex)
        wait_for(pc.status, lambda s: bool(s["snapshot"].get("vmic_exclusive")) == want_ex, 30, 1.0,
                 f"the PC's vmic_exclusive={want_ex}")
        wait_for(pc.status, lambda s: s["snapshot"].get("helper") == "ok", 30, 1.0,
                 "the PC helper back after its restart")
        time.sleep(3.0)
    # 2. lip sync in the PC helper: the policy, the backend's CABLE path; floor as shipped
    pc.command({"helper_dev": {"sync_offset_ms": None, "sync_audio_path_ms": None, "playout_floor_ms": 50,
                               "drop_udp": False}})
    # 3. the Mac: test source A on the mic, synthetic camera, time-map log, sharing on, 720p
    mac.command([{"camera_sharing": True}, {"set_quality": "720p"}, {"helper_dev": {"log_time_map": True}},
                 {"test_source": {"kind": "mic", "signal": "A:0.2"}}])
    wait_for(mac.status, lambda s: s.get("test_source") == "A:0.2", 15, 0.5, "the Mac's mic test source A")
    mac.command({"camera_test_source": "synth"})
    wait_for(mac.status, lambda s: (s.get("camera") or {}).get("test_source") == "synth", 15, 0.5,
             "the Mac's synthetic camera (a --dev helper with camera_synth)")
    # 4. routes: Mac mic -> the others' virtual mic, Mac camera -> the PC (both start on demand)
    mac.command([{"set_mic": {"machine": mac_id, "device": mac_mic}}, {"set_camera": {"machine": mac_id, "device": None}}])
    wait_for(pc.status, lambda s: (((s.get("camera") or {}).get("snapshot") or {}).get("route") or {})
             .get("camera", {}).get("machine") == mac_id, 30, 1.0, "the PC to hold the camera route")
    note(event="routes", mac=mac_id, pc=pc_id, mic=mac_mic)
    # 5. the PC half and the clock link
    pcrun = PC_HOME + "\\runs\\" + run_dir.parent.name + "-" + mode
    plan = {"dir": pcrun, "seconds": seconds, "lead": LEAD_S, "size": "1280x720"}
    (run_dir / "pc_plan.json").write_text(json.dumps(plan, indent=1))
    beamer.ssh(f"New-Item -ItemType Directory -Force {pcrun} | Out-Null")
    beamer.scp_to(run_dir / "pc_plan.json", pcrun + "\\plan.json")
    clock = ClockLink(["ssh", beamer.PC, f"{PC_PY} -u {PC_HOME}\\clock.py serve"]).start()
    t0 = time.monotonic()
    pcside = subprocess.Popen(["ssh", beamer.PC, f"{PC_PY} {PC_HOME}\\cam_lipsync.py pcside {pcrun}\\plan.json"],
                              stdout=open(run_dir / "pcside.out", "w"), stderr=subprocess.STDOUT)
    status_log = open(run_dir / "pc_status.jsonl", "w")
    end = t0 + LEAD_S + seconds + 3
    while time.monotonic() < end and pcside.poll() is None:
        st = pc.status()
        if st:
            status_log.write(json.dumps({"t": time.monotonic(), "camera_stats": (st.get("camera") or {}).get("stats"),
                                         "stats": st.get("stats")}) + "\n")
            status_log.flush()
        time.sleep(5.0)
    try:
        pcside.wait(timeout=120)
    except subprocess.TimeoutExpired:
        pcside.kill()
        note(event="pcside_killed")
    t_end = time.monotonic()
    pings = clock.stop()
    # 6. routes off, sources off
    mac.command([{"set_mic": {"machine": None, "device": None}}, {"set_camera": {"machine": None, "device": None}},
                 {"test_source": {"kind": "mic", "signal": None}}, {"helper_dev": {"log_time_map": False}}])
    time.sleep(1.5)
    mac.command({"camera_test_source": None})
    note(event="routes_off")
    meta = {"mode": mode, "vmic_exclusive": want_ex, "seconds": seconds, "t0": t0, "t_rec": t0 + LEAD_S,
            "t_end": t_end, "mac_id": mac_id, "pc_id": pc_id, "pcrun": pcrun,
            "started": time.strftime("%Y-%m-%d %H:%M:%S")}
    (run_dir / "meta.json").write_text(json.dumps(meta, indent=1))
    (run_dir / "pings.json").write_text(json.dumps({"pings": pings}))
    # 7. collect
    time.sleep(2.0)
    mac.copy_events(run_dir / "mac_events.jsonl")
    pc.copy_events(run_dir / "pc_events.jsonl")
    if beamer.scp_from(pcrun, run_dir / "pc_raw") != 0:
        note(event="collect_failed", what=pcrun)


def run(args):
    beamer, ClockLink = _mac_imports()
    try:
        beamer.ssh("exit", timeout=10)
    except Exception as error:  # noqa: BLE001
        raise Stop(f"lebig does not answer over ssh ({error}): switch it on, then run again")
    user = alex_signed_in(beamer)
    if not user:
        raise Stop("H8: nobody is signed in on lebig (camprobe must run in Alex's session): sign in, then run again")
    pc = beamer.Pc()
    pst = pc.status()
    if not pst:
        raise Stop("the PC's audio-dev-status.json is missing (Beamer in dev mode on the PC?)")
    was_ex = bool(pst["snapshot"].get("vmic_exclusive"))
    name = args.name or time.strftime("%Y%m%d-%H%M%S")
    top = RUNS / name
    top.mkdir(parents=True, exist_ok=True)
    locks = take_locks(args.ignore_audio_lock)
    notes = [{"t": time.monotonic(), "event": "start", "pc_user": user, "vmic_exclusive_before": was_ex}]
    results = {}
    try:
        for mode in args.modes.split(","):
            if mode not in MODES:
                raise Stop(f"unknown mode {mode!r} ({', '.join(MODES)})")
            print(f"cam_lipsync {mode}: {args.seconds} s")
            run_mode(beamer, ClockLink, mode, args.seconds, top / mode, notes)
            results[mode] = analyze_mode(top / mode)
            print_mode(results[mode])
    finally:
        try:
            if bool((pc.status() or {}).get("snapshot", {}).get("vmic_exclusive")) != was_ex:
                pc.command({"set_vmic_exclusive": was_ex})
                notes.append({"t": time.monotonic(), "event": "vmic_exclusive_restored", "on": was_ex})
        finally:
            (top / "notes.jsonl").write_text("".join(json.dumps(n) + "\n" for n in notes))
            release(locks)
    write_summary(top, results)


# =========================================================================== analysis (Mac, numpy)

def _jl(path):
    out = []
    if not Path(path).exists():
        return out
    for line in open(path, errors="replace"):
        line = line.strip()
        if line.startswith("{"):
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
    return out


def _sig():
    sys.path.insert(0, str(SCEN))
    import sig  # noqa: E402
    return sig


def pct(v, q):
    import numpy as np
    return round(float(np.quantile(np.asarray(v, float), q)), 1) if len(v) else None


def audio_sources(mac_ev, lo, hi, sigmod, signal="A"):
    """[(t_mac, k)] capture time of every marker of the Mac helper's test source in [lo, hi] (Mac monotonic),
    + the clock check: helper now_ns vs the app's monotonic stamp, and the time-map check."""
    stats = [e for e in mac_ev if e.get("ev") == "stats" and "now_ns" in e and lo - 5 <= e.get("mono", -1) <= hi + 5]
    if not stats:
        return [], {"error": "no Mac helper stats in the run window"}
    # The engine's now_ns (steady_clock) and the app's time.monotonic() (mach_absolute_time) should be one clock;
    # the minimum of (stamp - now_ns) is then the pipe + stamping delay (~ms). Anything large means the audio
    # time map (steady_clock) and the video one (backend host clock) are in different domains (a contract issue:
    # the engine's lip sync would be off by that much).
    d = min(e["mono"] - e["now_ns"] / 1e9 for e in stats)
    same = abs(d) < 0.05
    off = 0.0 if same else d
    segs = {}
    for e in stats:
        for sid, s in e.get("streams", {}).items():
            if s.get("dir") == "send" and s.get("kind") == "mic" and s.get("test_t0_ns"):
                key = (sid, s["test_t0_ns"])
                segs[key] = (min(segs.get(key, (1e18, 0))[0], e["mono"]), max(segs.get(key, (0, 0))[1], e["mono"]))
    inj = sigmod.Injector(signal)
    out = []
    for (sid, t0ns), (first, last) in sorted(segs.items(), key=lambda kv: kv[0][1]):
        t0 = t0ns / 1e9 + off
        k = 0
        while True:
            t = t0 + inj.marker_index(k) / sigmod.RATE
            if t > last + 0.5:
                break
            if t >= lo:
                out.append((t, k, sid))
            k += 1
    # time-map check: every logged tx entry of a test-source mic stream against test_t0 + n / 48 kHz, with
    # n = ts - ts_of_sample_0 (estimated as the median); the spread is the time map's own error
    tm = []
    mic_ids = {sid for sid, _ in segs}
    for e in mac_ev:
        if e.get("ev") == "time_map" and e.get("dir") == "tx" and lo - 5 <= e.get("mono", -1) <= hi + 5:
            for x in e.get("entries", []):
                if str(x.get("stream")) in mic_ids:
                    tm.append((str(x["stream"]), int(x["ts"]), int(x["sender_ns"])))
    check = {"clock_same_domain": same, "helper_minus_app_ms": round(-d * 1000, 3), "segments": len(segs),
             "markers": len(out)}
    if tm:
        import numpy as np
        by = {}
        t0_of = {sid: t0ns for sid, t0ns in segs}
        for sid, ts, ns in tm:
            n_est = (ns - t0_of[sid]) * sigmod.RATE / 1e9
            by.setdefault(sid, []).append((ts - n_est) % (1 << 32))
        spreads = []
        for sid, v in by.items():
            v = np.unwrap(np.asarray(v) * 2 * np.pi / (1 << 32)) * (1 << 32) / (2 * np.pi)
            spreads.append(float((v.max() - v.min()) / sigmod.RATE * 1000))
        check["time_map_entries"] = len(tm)
        check["time_map_vs_test_t0_spread_ms"] = round(max(spreads), 3)
    else:
        check["time_map_entries"] = 0
    return out, check


def audio_arrivals(raw, sigmod, clock_to_mac):
    """[(t_mac, similarity)] of signal A markers as an app receives them from CABLE Output (PC QPC -> Mac)."""
    import numpy as np
    p = raw / "cable"
    if not (raw / "cable.f32").exists():
        return [], {"error": "no cable.f32"}
    x = np.fromfile(raw / "cable.f32", "<f4")
    ch = np.fromfile(raw / "cable.chunks", "<f8").reshape(-1, 2)
    tm = sigmod.time_map(ch)
    det = sigmod.detect(x, "A", 0.4) if len(x) <= 120 * sigmod.RATE else _detect_long(x, sigmod)
    out = [(float(clock_to_mac(tm(j))), s) for j, s, _ in det]
    return out, {"samples": int(len(x)), "detections": len(det), "lateness_ms": round(tm.lateness_ms, 2),
                 "span_pc": [float(tm(0)), float(tm(len(x) - 1))] if len(x) else None, "path": str(p)}


def _detect_long(x, sigmod, piece_s=60):
    piece = piece_s * sigmod.RATE
    out = []
    for a in range(0, len(x), piece):
        for j, s, amp in sigmod.detect(x[a:a + piece + sigmod.RATE], "A", 0.4):
            j += a
            if out and j - out[-1][0] < 0.3 * sigmod.RATE:
                if s > out[-1][1]:
                    out[-1] = (j, s, amp)
                continue
            out.append((j, s, amp))
    return out


def video_frames(raw, t_ref_mac, clock_to_mac):
    """[(capture t_mac, arrival t_mac, frame)] of c1 pictures camprobe read (wall clock -> QPC -> Mac)."""
    import numpy as np
    pairs = _jl(raw / "clockpairs.jsonl")
    dump = raw / "cam.dump"
    if not dump.exists() or len(pairs) < 2:
        return [], {"error": "no cam.dump or no clock pairs (did the agent run in Alex's session?)"}
    P = np.array([(p["unix"], p["mono"]) for p in pairs if p.get("spread", 0) < 0.002], float)
    b, a = np.polyfit(P[:, 0] - P[0, 0], P[:, 1], 1)  # mono = a + b (unix - unix0)
    res = P[:, 1] - (a + b * (P[:, 0] - P[0, 0]))
    out, classes = [], {}
    for line in open(dump):
        f = line.split()
        if len(f) != 4:
            continue
        classes[f[1]] = classes.get(f[1], 0) + 1
        if f[1] != "c1":
            continue
        t_unix = int(f[0]) * 1e-6
        mono = a + b * (t_unix - P[0, 0])
        ts = int(f[3]) * 1e-4
        cap = ts + round((t_ref_mac - ts) / STRIP_WRAP_S) * STRIP_WRAP_S
        out.append((cap, float(clock_to_mac(mono)), int(f[2])))
    return out, {"pictures": sum(classes.values()), "classes": classes, "wall_to_qpc_resid_ms": round(float(res.std()) * 1e3, 3),
                 "wall_to_qpc_slope_ppm": round((b - 1) * 1e6, 2)}


def pair_offsets(src, arr, frames, window=0.6, frame_win=0.25):
    """Per audio marker: (t_mac capture, audio latency ms, video latency ms, offset ms). Latencies include the
    residual Mac/PC clock error, which cancels in the offset."""
    import numpy as np
    at = np.array([a[0] for a in arr]) if arr else np.zeros(0)
    used = np.zeros(len(at), bool)
    fc = np.array([f[0] for f in frames]) if frames else np.zeros(0)
    fl = np.array([(f[1] - f[0]) * 1000 for f in frames]) if frames else np.zeros(0)
    order = np.argsort(fc)
    fc, fl = fc[order], fl[order]
    rows, missed = [], 0
    for t, k, sid in src:
        m = np.nonzero((at > t) & (at < t + window) & ~used)[0]
        if not len(m):
            missed += 1
            continue
        used[m[0]] = True
        a_ms = (at[m[0]] - t) * 1000
        lo, hi = np.searchsorted(fc, t - frame_win), np.searchsorted(fc, t + frame_win)
        if hi - lo < 3:
            continue
        v_ms = float(np.median(fl[lo:hi]))
        rows.append((t, a_ms, v_ms, a_ms - v_ms))
    return rows, missed


def engine_view(pc_ev, lo_pc, hi_pc):
    """The PC helper's own lip-sync numbers (video recv stats) over the measured window (PC monotonic)."""
    rows = []
    for e in pc_ev:
        if e.get("ev") != "stats" or not (lo_pc <= e.get("mono", -1) <= hi_pc):
            continue
        for sid, s in e.get("streams", {}).items():
            if s.get("kind") == "video" and s.get("dir") == "recv":
                rows.append(s)
    if not rows:
        return {"error": "no PC video recv stats in the window"}
    if not any("sync_audio_ms" in r for r in rows):
        return {"error": "the PC helper predates CW10 (no sync_audio_ms in its video stats)", "n": len(rows)}

    def col(k):
        return [r[k] for r in rows if isinstance(r.get(k), (int, float))]
    return {"n": len(rows), "sync_ms_p50": pct(col("sync_ms"), 0.5), "sync_ms_min": pct(col("sync_ms"), 0),
            "sync_ms_max": pct(col("sync_ms"), 1), "sync_audio_ms_p50": pct(col("sync_audio_ms"), 0.5),
            "sync_skew_ms_p50": pct(col("sync_skew_ms"), 0.5), "sync_path_ms": pct(col("sync_path_ms"), 0.5),
            "video_present_delay_ms_p50": pct(col("video_present_delay_ms"), 0.5),
            "target_ms_p50": pct(col("target_ms"), 0.5), "offset_ms_p50": pct(col("offset_ms"), 0.5),
            "unpaired_stats": sum(1 for r in rows if r.get("sync_audio_ms") is None)}


def analyze_mode(d: Path):
    sigmod = _sig()
    import numpy as np  # noqa: F401
    meta = json.load(open(d / "meta.json"))
    pings = json.load(open(d / "pings.json"))["pings"]
    if len(pings) < 10:
        return {"mode": meta["mode"], "error": "too few clock pings"}
    off, clock = sigmod.clock_fit(pings)  # off(t_mac) = PC - Mac

    def to_mac(t_pc):
        t_pc = np.asarray(t_pc, float)
        return t_pc - off(t_pc - off(t_pc))
    raw = d / "pc_raw"
    lo = meta["t_rec"] + SETTLE_S
    hi = meta["t_rec"] + meta["seconds"] - 1.0
    src, acheck = audio_sources(_jl(d / "mac_events.jsonl"), lo, hi, sigmod)
    arr, ainfo = audio_arrivals(raw, sigmod, to_mac)
    frames, vinfo = video_frames(raw, (lo + hi) / 2, to_mac)
    src = [s for s in src if lo <= s[0] <= hi]
    rows, missed = pair_offsets(src, arr, frames)
    offs = [r[3] for r in rows]
    a_lat = [r[1] for r in rows]
    v_lat = [r[2] for r in rows]
    eng = engine_view(_jl(d / "pc_events.jsonl"), float(lo + off(lo)), float(hi + off(hi)))
    p50 = pct(offs, 0.5)
    ok = p50 is not None and PASS_LO <= p50 <= PASS_HI and len(rows) >= 0.8 * max(1, len(src))
    res = {"mode": meta["mode"], "vmic_exclusive": meta["vmic_exclusive"], "window_s": round(hi - lo, 1),
           "markers": len(src), "paired": len(rows), "missed": missed,
           "offset_ms": {"p50": p50, "p05": pct(offs, 0.05), "p95": pct(offs, 0.95), "min": pct(offs, 0),
                         "max": pct(offs, 1), "in_window_pct": round(100.0 * sum(PASS_LO <= o <= PASS_HI for o in offs)
                                                                     / max(1, len(offs)), 1)},
           "audio_capture_to_app_ms": {"p50": pct(a_lat, 0.5), "p95": pct(a_lat, 0.95)},
           "video_capture_to_app_ms": {"p50": pct(v_lat, 0.5), "p95": pct(v_lat, 0.95)},
           "pass": bool(ok), "mark": f"offset p50 within [{PASS_LO:+.0f}, {PASS_HI:+.0f}] ms, >= 80 % of markers paired",
           "engine": eng, "clock": clock, "audio_source_check": acheck, "audio_sink": ainfo, "video": vinfo}
    (d / "result.json").write_text(json.dumps(res, indent=1))
    return res


def print_mode(r):
    if r.get("error"):
        print(f"  {r['mode']}: ERROR {r['error']}")
        return
    o = r["offset_ms"]
    e = r.get("engine", {})
    print(f"  {r['mode']}: {'PASS' if r['pass'] else 'FAIL'}  offset p50 {o['p50']} ms (p05 {o['p05']}, p95 {o['p95']}; "
          f"{o['in_window_pct']} % in window), {r['paired']}/{r['markers']} markers; audio {r['audio_capture_to_app_ms']['p50']} "
          f"ms, video {r['video_capture_to_app_ms']['p50']} ms capture -> app; engine hold {e.get('sync_ms_p50')} ms, "
          f"skew {e.get('sync_skew_ms_p50')} ms{(' (' + e['error'] + ')') if e.get('error') else ''}; "
          f"clock same domain {r['audio_source_check'].get('clock_same_domain')}")


def write_summary(top: Path, results):
    out = {"case": "cam_lipsync", "pass": bool(results) and all(r.get("pass") for r in results.values()),
           "mark": f"A/V offset within [{PASS_LO:+.0f}, {PASS_HI:+.0f}] ms (audio later = positive) in shared and "
                   f"low-latency mode", "modes": results}
    (top / "result.json").write_text(json.dumps(out, indent=1))
    print(f"cam_lipsync: {'PASS' if out['pass'] else 'FAIL'} -> {top / 'result.json'}")


def analyze(path):
    top = Path(path)
    if (top / "meta.json").exists():  # one mode's folder
        print_mode(analyze_mode(top))
        return
    results = {}
    for d in sorted(p for p in top.iterdir() if (p / "meta.json").exists()):
        results[d.name] = analyze_mode(d)
        print_mode(results[d.name])
    write_summary(top, results)


# =========================================================================== selftest (offline)

def selftest():
    """Synthetic run folders through the whole analysis: known audio and video delays, a PC clock 3 h ahead
    of the Mac's and drifting 8 ppm, a PC wall clock 1.6e9 s off its QPC; the offset must come back exact."""
    import shutil
    import tempfile

    import numpy as np
    sigmod = _sig()
    rate = sigmod.RATE
    ok = True
    for audio_ms, video_ms in ((172.0, 160.0), (120.0, 165.0), (230.0, 150.0)):
        d = Path(tempfile.mkdtemp(prefix="lipsync-selftest-"))
        raw = d / "pc_raw"
        raw.mkdir()
        seconds = 60
        mac_t0 = 1_000_000.0                  # Mac monotonic at the run start (11.6 days up: the strip wraps)
        pc_of = lambda t: t + 3 * 3600 + 8e-6 * (t - mac_t0)  # noqa: E731  (PC QPC of a Mac time)
        unix_of = lambda q: q + 1.6e9 + 0.25  # noqa: E731  (PC wall of a PC QPC time)
        t_rec = mac_t0 + LEAD_S
        # Mac helper: test source sample 0 at t_rec - 10 s; stats every 1 s, stamped 2 ms after
        t0ns = int((t_rec - 10.0) * 1e9)
        ev = []
        for i in range(seconds + 20):
            m = t_rec - 12 + i
            ev.append({"ev": "stats", "mono": m + 0.002, "now_ns": int(m * 1e9),
                       "streams": {"11": {"dir": "send", "kind": "mic", "test_t0_ns": t0ns}}})
        ts0 = 123456789
        for i in range(0, seconds + 10, 1):
            n = i * rate
            ev.append({"ev": "time_map", "dir": "tx", "mono": t_rec - 10 + i + 0.001,
                       "entries": [{"stream": 11, "ts": (ts0 + n) % (1 << 32), "sender_ns": t0ns + int(n * 1e9 / rate)}]})
        (d / "mac_events.jsonl").write_text("".join(json.dumps(e) + "\n" for e in ev))
        # CABLE Output: the test signal delayed by audio_ms, recorded from t_rec on (PC QPC chunks of 10 ms)
        inj = sigmod.Injector("A")
        n_rec = (seconds + 2) * rate
        s_first = int(round((t_rec - (t0ns / 1e9) - audio_ms / 1000) * rate))  # source sample at the recording start
        x, _ = inj.render(s_first, n_rec)
        x = x + np.random.default_rng(1).normal(0, 1e-4, n_rec).astype(np.float32)
        x.astype("<f4").tofile(raw / "cable.f32")
        ends = np.arange(480, n_rec + 1, 480)
        read_t = np.array([pc_of(t_rec + e / rate) for e in ends]) + np.random.default_rng(2).uniform(0, 0.004, len(ends))
        np.stack([ends.astype(float), read_t], 1).astype("<f8").tofile(raw / "cable.chunks")
        # camera: 30 fps captured on the Mac, received video_ms later; strip time 100 us, wrapped
        lines, pairs = [], []
        for f in range(int((seconds + 2) * 30)):
            cap = t_rec + f / 30.0
            q = pc_of(cap + video_ms / 1000 + np.random.default_rng(f).uniform(-0.002, 0.002))
            ts = int(round(cap * 1e4)) % (1 << 32)
            lines.append(f"{int(round(unix_of(q) * 1e6))} c1 {f} {ts}\n")
        (raw / "cam.dump").write_text("".join(lines))
        for i in range(int(seconds * 4)):
            q = pc_of(t_rec) + i * 0.25
            pairs.append({"mono": q, "unix": unix_of(q), "spread": 1e-5})
        (raw / "clockpairs.jsonl").write_text("".join(json.dumps(p) + "\n" for p in pairs))
        # clock pings (Mac t, rtt, PC - Mac) and the PC helper's stats
        pings = [(mac_t0 + i * 0.25, 0.004 + 0.002 * (i % 3), pc_of(mac_t0 + i * 0.25) - (mac_t0 + i * 0.25))
                 for i in range(int(seconds * 4 + 40))]
        (d / "pings.json").write_text(json.dumps({"pings": pings}))
        pc_ev = [{"ev": "stats", "mono": pc_of(t_rec + i), "streams": {"507": {
            "dir": "recv", "kind": "video", "sync_ms": 60.0, "sync_audio_ms": 150.0, "sync_skew_ms": 0.5,
            "sync_path_ms": 67.0, "video_present_delay_ms": 149.5, "target_ms": 50.0, "offset_ms": -1e7}}}
            for i in range(seconds)]
        (d / "pc_events.jsonl").write_text("".join(json.dumps(e) + "\n" for e in pc_ev))
        (d / "meta.json").write_text(json.dumps({"mode": "shared", "vmic_exclusive": False, "seconds": seconds,
                                                 "t0": mac_t0, "t_rec": t_rec, "t_end": t_rec + seconds + 3}))
        r = analyze_mode(d)
        want = audio_ms - video_ms
        got = r["offset_ms"]["p50"]
        good = (got is not None and abs(got - want) <= 1.5 and r["paired"] >= 0.95 * r["markers"] > 20
                and r["audio_source_check"]["clock_same_domain"]
                and r["audio_source_check"]["time_map_vs_test_t0_spread_ms"] < 0.1
                and abs(r["audio_capture_to_app_ms"]["p50"] - audio_ms) <= 2.0
                and abs(r["video_capture_to_app_ms"]["p50"] - video_ms) <= 2.0
                and r["pass"] == (PASS_LO <= want <= PASS_HI) and r["engine"].get("sync_ms_p50") == 60.0)
        print(f"selftest audio {audio_ms} video {video_ms}: offset {got} (want {want:+.1f}), paired {r['paired']}/"
              f"{r['markers']}, audio {r['audio_capture_to_app_ms']['p50']}, video {r['video_capture_to_app_ms']['p50']},"
              f" pass {r['pass']}: {'ok' if good else 'WRONG'}")
        ok = ok and good
        shutil.rmtree(d)
    print("selftest", "ok" if ok else "FAILED")
    return 0 if ok else 1


# =========================================================================== main

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("setup")
    r = sub.add_parser("run")
    r.add_argument("--modes", default="shared,lowlat")
    r.add_argument("--seconds", type=int, default=300)
    r.add_argument("--name")
    r.add_argument("--ignore-audio-lock", action="store_true")
    a = sub.add_parser("analyze")
    a.add_argument("path")
    sub.add_parser("selftest")
    p = sub.add_parser("pcside")
    p.add_argument("plan")
    g = sub.add_parser("agent")
    g.add_argument("plan")
    args = ap.parse_args()
    try:
        if args.cmd == "pcside":
            pc_side(args.plan)
        elif args.cmd == "agent":
            pc_agent(args.plan)
        elif args.cmd == "setup":
            setup()
        elif args.cmd == "run":
            run(args)
        elif args.cmd == "analyze":
            analyze(args.path)
        elif args.cmd == "selftest":
            sys.exit(selftest())
    except Stop as stop:
        print(f"cam_lipsync stopped: {stop}", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
