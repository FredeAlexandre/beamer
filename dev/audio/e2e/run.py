# SPDX-License-Identifier: GPL-3.0-only
"""Two-machine E2E runs of Beamer's audio (dev/audio/PHASE2.md §11.4), from this Mac, unattended.

    uv run --python 3.13 --with numpy dev/audio/e2e/run.py setup          # copy the PC half, build tools
    uv run --python 3.13 --with numpy dev/audio/e2e/run.py CASE [--duration S] [--name NAME]
    uv run --python 3.13 --with numpy dev/audio/e2e/run.py idle            # route back to idle on both

Both Beamer apps must run in dev mode (`audio-dev.enabled`, `{}` is enough) with the real helpers
(`dev/deploy.sh both`). Routes change through the dev command files exactly as the Audio page would
change them; nothing else in Beamer is touched. Hold `~/Developer/.beamer-audio-lab.lock` around runs.

Signals (proto/scenarios/sig.py): A = from the Mac, B = from the PC. Mic cases use the helper's dev
test source (the same Injector, rendered in the helper; its sample 0 time comes from the helper stats
`test_t0_ns`), so latency starts where a mic capture would deliver. Sound cases play the signal with
the Phase 1 tool and time it with an unmuted tap / endpoint loopback beside Beamer's own capture.
Sinks are recorded by the Phase 1 tools as an app would use them: Mac "Beamer Microphone", PC
"CABLE Output", Mac process tap of the speakers, PC endpoint loopback of the headset. Clocks are
joined by clock.py (ssh), analysis by results.py (reusing proto/scenarios/analyze.py).

Cases: mic_pc_mac mic_pc_mac_real sound_pc_mac sound_mac_pc mic_mac_pc mic_mac_pc_real both both_mic
fallback helperkill_pc helperkill_mac unplug_mac linkloss (= persist) oldpeer soak.
Output: runs/<name>/result.json.
"""
from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
SCEN = HERE.parent / "proto" / "scenarios"
sys.path.insert(0, str(HERE))
import beamer  # noqa: E402
from beamer import PC_HOME, PC_PY  # noqa: E402
from clock import ClockLink  # noqa: E402

RUNS = HERE / "runs"
TOOLS = RUNS / "tools"
MAC_TOOL = TOOLS / "proto-audio"
AGG = TOOLS / "agg"
PY = sys.executable
TAP = SCEN / "tap.py"
PC_TAP = PC_HOME + r"\tap.py"
PC_TOOL = PC_HOME + r"\proto-audio.exe"
PROTO_MAC_BUILD = Path.home() / "Developer" / "beamer" / "dev" / "audio" / "proto" / "mac" / ".build" / "beamer-audio"
PROTO_PC_BUILD = r"C:\Users\Alex\audio-proto\scenarios\beamer-audio.exe"

# Speaker-facing signals stay quiet (pilot -46 dBFS, chirp peak -30 dBFS): measurement is on digital
# taps/loopbacks, so level only matters to whoever is in the room.
QUIET = "--pilot-amp 0.005 --marker-amp 0.03"
LAB_LOCK = Path.home() / "Developer" / ".beamer-audio-lab.lock"
DEPLOY_LOCK = Path.home() / "Developer" / ".beamer-deploy.lock"

mac, pc = beamer.Mac(), beamer.Pc()


def setup():
    TOOLS.mkdir(parents=True, exist_ok=True)
    if not MAC_TOOL.exists():
        subprocess.run(["cp", str(PROTO_MAC_BUILD), str(MAC_TOOL)], check=True)
    subprocess.run(["clang", "-O2", "-framework", "CoreAudio", "-framework", "CoreFoundation",
                    str(HERE / "agg.c"), "-o", str(AGG)], check=True)
    beamer.ssh(f"New-Item -ItemType Directory -Force {PC_HOME}\\runs | Out-Null; "
               f"if (-not (Test-Path {PC_TOOL})) {{ Copy-Item {PROTO_PC_BUILD} {PC_TOOL} }}; "
               f"if (-not (Test-Path {PC_PY})) {{ Set-Location {PC_HOME}; uv venv -q --python 3.13 venv; "
               f"uv pip install -q --python venv\\Scripts\\python.exe numpy }}")
    for f in (SCEN / "tap.py", SCEN / "sig.py", HERE / "pcside.py", HERE / "clock.py", HERE / "pcplay.py",
              HERE / "pair_check.py"):
        beamer.scp_to(f, PC_HOME + "\\" + f.name)
    print("setup done")


# ---------------------------------------------------------------- cases

class Ctx:
    """Machine ids and device refs from both status files."""

    def __init__(self):
        sm, sp = mac.status(), pc.status()
        self.mac_status, self.pc_status = sm, sp
        self.mac_id = sm["snapshot"]["own"]
        self.pc_id = sp["snapshot"]["own"]
        s = sm
        self.wave = beamer.device(s, self.pc_id, "inputs", "Wave:3")
        self.g733 = beamer.device(s, self.pc_id, "outputs", "G733")
        try:  # only the cases that play on it need it; the controller is often off
            self.dualsense = beamer.device(s, self.pc_id, "outputs", "DualSense")
        except KeyError:
            self.dualsense = None
        self.mac_mic = beamer.device(s, self.mac_id, "inputs", "BuiltInMicrophoneDevice")
        self.mac_spk = beamer.device(s, self.mac_id, "outputs", "BuiltInSpeakerDevice")
        sp_m = sp["snapshot"]["machines"][self.pc_id]
        self.pc_default_out = sp_m.get("default_output")


def mac_rec(name, seconds, capture, channels):
    return f"{shlex.quote(PY)} {shlex.quote(str(TAP))} rec --out @RUN@/{name} --channels {channels} --seconds {seconds} -- {MAC_TOOL} {capture}"


def pc_rec(name, seconds, capture, channels):
    return f"{PC_PY} {PC_TAP} rec --out @PCRUN@\\{name} --channels {channels} --seconds {seconds} -- {PC_TOOL} {capture}"


def mac_gen(sig, seconds, delay=0):
    return (f"{shlex.quote(PY)} {shlex.quote(str(TAP))} gen --out @RUN@/inj --sig {sig} --channels 2 --seconds {seconds} "
            f"--delay {delay} {QUIET} | {MAC_TOOL} play --channels 2 --buffer-frames 128 --prefill-ms 15")


def pc_gen(sig, seconds, device_key):
    """A player in the PC's signed-in session (pcside.py `session1`). The G733 is a headset nobody
    wears, muted by Beamer while its sound is sent: Phase 1 levels (pilot -34 dBFS)."""
    return {"gen": [PC_PY, PC_TAP, "gen", "--out", "@PCRUN@\\inj", "--sig", sig, "--channels", "2",
                    "--seconds", str(seconds)],
            "play": [PC_TOOL, "play", "--device", device_key, "--channels", "2", "--live-drop-ms", "60",
                     "--queue-ms", "20"]}


def cases(c: Ctx, D: float):
    """name -> spec. Times are seconds from the run start; the route is set at T_ROUTE and put back to
    idle at D; recorders run from 0.5 s to D + 3."""
    R = D + 3
    mic_pc = {"set_mic": {"machine": c.pc_id, "device": c.wave}}
    mic_mac = {"set_mic": {"machine": c.mac_id, "device": c.mac_mic}}
    pc_to_mac = {"set_output": {"machine": c.pc_id, "to": c.mac_id, "device": None}}
    mac_to_g733 = {"set_output": {"machine": c.mac_id, "to": c.pc_id, "device": c.g733}}
    vmic_rec = mac_rec("vmic", R - 4, 'capture-mic --device "Beamer Microphone" --channels 1 --buffer-frames 128', 1)
    cable_rec = pc_rec("cable", R - 4, f"capture-mic --device {CABLE_OUT} --channels 1", 1)
    g733_loop = pc_rec("g733", R, f"capture-system --device {c.g733['key']} --channels 2", 2)
    mac_tap = mac_rec("tap", R, "capture-system --channels 2", 2)
    poll_g733 = {"period": 0.15, "match": ["G733", "DualSense"]}
    S = {}
    S["mic_pc_mac"] = dict(
        desc="(a) PC mic -> Mac apps via Beamer Microphone (helper test source B; recorder opens at 5 s: on demand)",
        route=[("pc", {"test_source": {"kind": "mic", "signal": "B:0.2"}}), ("mac", mic_pc)],
        mac=[(5.0, "vmic", vmic_rec)], pc=[], pairs=[("pc/testsrc", "mac/vmic", "B")], poll=None, demand="mac/vmic")
    S["mic_pc_mac_real"] = dict(
        desc="(a') PC Wave:3 (real device, room sound) -> Mac Beamer Microphone; PC records the Wave:3 beside it",
        route=[("mac", mic_pc)],
        mac=[(5.0, "vmic", vmic_rec)],
        pc=[(0.5, "wave", pc_rec("wave", R, f"capture-mic --device {c.wave['key']} --channels 1", 1))],
        pairs=[], poll=None, demand="mac/vmic", envelope=("pc/wave", "mac/vmic"))
    S["sound_pc_mac"] = dict(
        desc="(b) PC sound (B played on the G733) -> Mac speakers, PC muted locally",
        route=[("mac", pc_to_mac)],
        mac=[(0.5, "tap", mac_tap)], pc=[(0.5, "g733", g733_loop), (1.0, "gen", pc_gen("B", R - 1.5, c.g733["key"]))],
        pairs=[("pc/inj", "pc/g733", "B"), ("pc/g733", "mac/tap", "B")], poll=poll_g733, mac_mute=True)
    S["sound_mac_pc"] = dict(
        desc="(c) Mac sound (A) -> PC G733, Mac muted locally (Mac mic listens for the speakers)",
        route=[("mac", mac_to_g733)],
        mac=[(0.5, "tap", mac_tap), (1.0, "gen", mac_gen("A", R - 1.5)),
             (0.5, "macmic", mac_rec("macmic", R, "capture-mic --channels 1", 1))],
        pc=[(0.5, "g733", g733_loop)],
        pairs=[("mac/inj", "mac/tap", "A"), ("mac/tap", "pc/g733", "A")], poll=poll_g733, mute_check="mac/macmic")
    S["mic_mac_pc"] = dict(
        desc="(d) Mac mic -> PC apps via CABLE Output (helper test source A; recorder opens at 5 s: on demand)",
        route=[("mac", {"test_source": {"kind": "mic", "signal": "A:0.2"}}), ("mac", mic_mac)],
        mac=[], pc=[(5.0, "cable", cable_rec)], pairs=[("mac/testsrc", "pc/cable", "A")], poll=None, demand="pc/cable")
    S["mic_mac_pc_real"] = dict(
        desc="(d') Mac built-in mic (real capture inside Beamer: TCC Microphone) -> PC CABLE Output",
        route=[("mac", mic_mac)],
        mac=[(0.5, "macmic", mac_rec("macmic", R, "capture-mic --channels 1", 1))],
        pc=[(5.0, "cable", cable_rec)], pairs=[], poll=None, demand="pc/cable", envelope=("mac/macmic", "pc/cable"))
    if c.dualsense is None:
        S["both"] = dict(desc="(e) needs the PC's DualSense", missing="the PC's DualSense output (turn the controller "
                                                                     "on and plug it in, then run again)")
    else:
        mac_to_ds = {"set_output": {"machine": c.mac_id, "to": c.pc_id, "device": c.dualsense}}
        S["both"] = dict(
            desc="(e) Mac sound (A) -> PC DualSense and PC sound (B, on the G733) -> Mac speakers at once; "
                 "first 6 s Mac -> G733 (expected: conflict)",
            route=[("mac", pc_to_mac), ("mac", mac_to_g733)],
            actions=[(8.0, "mac", "command", mac_to_ds)],
            mac=[(0.5, "tap", mac_tap), (1.0, "gen", mac_gen("A", R - 1.5))],
            pc=[(0.5, "g733", g733_loop),
                (0.5, "ds", pc_rec("ds", R, f"capture-system --device {c.dualsense['key']} --channels 2", 2)),
                (1.0, "gen", pc_gen("B", R - 1.5, c.g733["key"]))],
            pairs=[("mac/inj", "mac/tap", "A"), ("mac/tap", "pc/ds", "A"), ("pc/inj", "pc/g733", "B"),
                   ("pc/g733", "mac/tap", "B")],
            poll=poll_g733, loopcheck=True, mac_mute=True)
    # Recovery cases carry PC mic (test B) -> Mac "Beamer Microphone" and PC sound -> Mac speakers at
    # once: the vmic recording times the outage without a Mac tap (taps wait on TCC while a Beamer
    # prompt is pending), the G733 mute timeline shows the PC's local sound coming back.
    # The PC's test mic uses signature A here: the Mac's global tap also hears what the helper renders
    # into the hidden "Beamer Mic Feed", and two copies of one pilot would beat (seen in hk_pc_2).
    both_pc = dict(
        route=[("pc", {"test_source": {"kind": "mic", "signal": "A:0.2"}}), ("mac", mic_pc), ("mac", pc_to_mac)],
        mac=[(0.5, "tap", mac_tap), (1.0, "vmic", vmic_rec)],
        pc=[(0.5, "g733", g733_loop), (1.0, "gen", pc_gen("B", R - 1.5, c.g733["key"]))],
        pairs=[("pc/testsrc", "mac/vmic", "A"), ("pc/inj", "pc/g733", "B"), ("pc/g733", "mac/tap", "B")],
        poll=poll_g733, mac_mute=True)
    S["helperkill_pc"] = dict(both_pc, actions=[(25.0, "pc", "kill_helper", None)],
                              desc="helper kill: PC mic + PC sound -> Mac, PC helper killed at 25 s "
                                   "(guard/journal unmute, watchdog restart, streams reopened)")
    S["helperkill_mac"] = dict(both_pc, actions=[(25.0, "mac", "kill_helper", None)],
                               desc="helper kill: PC mic + PC sound -> Mac, Mac helper killed at 25 s")
    S["fallback"] = dict(both_pc, desc="fallback: PC mic + PC sound -> Mac with UDP dropped by the PC helper "
                                       "(dev drop_udp) from 20 s to 50 s",
                         actions=[(20.0, "pc", "command", {"helper_dev": {"drop_udp": True}}),
                                  (50.0, "pc", "command", {"helper_dev": {"drop_udp": False}})])
    up = dict(S["sound_pc_mac"])
    up.update(desc="unplug: (b) into a Mac aggregate output 'E2E Unplug' destroyed at 25 s and re-created at 45 s",
              route=[("mac", {"set_output": {"machine": c.pc_id, "to": c.mac_id,
                                             "device": {"key": AGG_UID, "name": "E2E Unplug"}}})],
              actions=[(25.0, "mac", "agg_off", None), (45.0, "mac", "agg_on", None)], agg=True)
    S["unplug_mac"] = up
    S["linkloss"] = dict(both_pc, actions=[(20.0, "mac", "redeploy", None)],
                         desc="link loss + persist: PC mic + PC sound -> Mac while the Mac app quits and "
                              "restarts (dev/deploy.sh mac at 20 s)")
    S["both_mic"] = dict(
        desc="(e') Mac mic (test A) -> PC CABLE Output and PC sound (B) -> Mac speakers at once: the PC "
             "helper renders A into CABLE Input inside the tree its own capture excludes (loop check)",
        route=[("mac", {"test_source": {"kind": "mic", "signal": "A:0.2"}}), ("mac", mic_mac), ("mac", pc_to_mac)],
        mac=[(0.5, "tap", mac_tap)],
        pc=[(5.0, "cable", cable_rec), (0.5, "g733", g733_loop), (1.0, "gen", pc_gen("B", R - 1.5, c.g733["key"]))],
        pairs=[("mac/testsrc", "pc/cable", "A"), ("pc/inj", "pc/g733", "B"), ("pc/g733", "mac/tap", "B")],
        poll=poll_g733, mac_mute=True, loopcheck=True)
    soak = dict(
        desc="soak: Mac mic (test A) -> PC CABLE Output and PC sound (B) -> Mac speakers, both at once",
        route=[("mac", {"test_source": {"kind": "mic", "signal": "A:0.2"}}), ("mac", mic_mac), ("mac", pc_to_mac)],
        mac=[(0.5, "tap", mac_tap)],
        pc=[(5.0, "cable", cable_rec), (0.5, "g733", g733_loop), (1.0, "gen", pc_gen("B", R - 1.5, c.g733["key"]))],
        pairs=[("mac/testsrc", "pc/cable", "A"), ("pc/inj", "pc/g733", "B"), ("pc/g733", "mac/tap", "B")],
        poll={"period": 5.0, "match": ["G733"]}, resources_every=60, mac_mute=True)
    S["soak"] = soak
    return S


CABLE_OUT = "{0.0.1.00000000}.{ddc7e301-3260-4ed3-8389-0938254820e0}"
AGG_UID = "com.beamer.e2e.unplug"
T_ROUTE = 2.0
TEST_SOURCE_WAIT = 15.0  # s: the PC's 1 s dev poll + its status write + scp/ssh round trips (~1.5 s seen)


def set_pc_test_source(cmds, notes, timeout=TEST_SOURCE_WAIT):
    """Writes the PC's `test_source` command(s) and waits until its audio-dev-status.json names the
    source in force, so the route set next opens the PC's stream on it at once. Without the wait the
    route could reach the PC first: its mic opened on the real device, then again on the test source
    (WP13, the double mic open)."""
    want = [c["test_source"].get("signal") for c in cmds if "test_source" in c][-1]
    started = time.monotonic()
    pc.command(cmds)
    seen = None
    while time.monotonic() - started < timeout:
        status = pc.status()
        seen = status.get("test_source") if status else None
        if status is not None and seen == want:
            notes.append({"t": time.monotonic(), "event": "pc_test_source", "signal": want,
                          "waited_s": round(time.monotonic() - started, 2)})
            return
        time.sleep(0.2)
    raise RuntimeError(f"the PC did not report test_source {want!r} within {timeout:.0f} s (its "
                       f"audio-dev-status.json says {seen!r}): is Beamer running there in dev mode "
                       f"(audio-dev.enabled) with a build that writes `test_source`?")


# ---------------------------------------------------------------- one run

class MacProcs:
    def __init__(self, run_dir):
        self.run = run_dir
        self.procs = []

    def spawn(self, name, cmd):
        cmd = cmd.replace("@RUN@", str(self.run))
        err = open(self.run / f"{name}.err", "ab")
        p = subprocess.Popen(cmd, shell=True, stdout=subprocess.DEVNULL, stderr=err, start_new_session=True)
        self.procs.append((name, p))
        return p

    def wait(self, timeout):
        end = time.monotonic() + timeout
        for name, p in self.procs:
            try:
                p.wait(timeout=max(0.1, end - time.monotonic()))
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, 15)
                print(f"mac: {name} still running, terminated")


def _pc_proc(name, cmd, delay, pcrun):
    if isinstance(cmd, dict):
        return {"name": name, "delay": delay,
                "session1": {k: [a.replace("@PCRUN@", pcrun) for a in v] for k, v in cmd.items()}}
    return {"name": name, "cmd": cmd.replace("@PCRUN@", pcrun), "delay": delay}


def mac_output_muted():
    out = subprocess.run(["osascript", "-e", "output muted of (get volume settings)"], capture_output=True, text=True)
    return out.stdout.strip() == "true"


def set_mac_output_muted(on):
    subprocess.run(["osascript", "-e", f"set volume output muted {'true' if on else 'false'}"], check=True)
    if mac_output_muted() != on:
        raise RuntimeError("Mac output mute did not take")


def run_case(name, spec, D, run_dir, notes):
    """Mac speakers are muted (system output mute, restored after) whenever the Mac plays received PC
    sound: the PC's Wave:3 hears the MacBook speakers, and Windows process loopback also captures
    Elgato Wave Link's monitor mix of that mic, so PC sound -> Mac speakers howls (first run b_1).
    Taps capture before the device's mute, so the measurement is unchanged."""
    was_muted = None
    if spec.get("mac_mute"):
        was_muted = mac_output_muted()
        set_mac_output_muted(True)
        notes.append({"t": time.monotonic(), "event": "mac_output_muted", "was": was_muted})
    try:
        _run_case(name, spec, D, run_dir, notes)
    finally:
        if was_muted is not None:
            set_mac_output_muted(was_muted)
            notes.append({"t": time.monotonic(), "event": "mac_output_restored", "muted": mac_output_muted()})
            (run_dir / "notes.jsonl").write_text("".join(json.dumps(n) + "\n" for n in notes))


def _run_case(name, spec, D, run_dir, notes):
    t0 = time.monotonic()
    meta = {"name": run_dir.name, "scenario": name, "desc": spec["desc"], "duration": D, "pairs": spec["pairs"],
            "started": time.strftime("%Y-%m-%d %H:%M:%S"), "t0": t0, "t_send_start": t0 + T_ROUTE,
            "spec": {k: v for k, v in spec.items() if k not in ("mac", "pc")},
            "test_signal": {m: c["test_source"]["signal"] for m, c in spec["route"] if "test_source" in c},
            "mac_cmds": spec["mac"], "pc_cmds": spec["pc"]}
    pcrun = PC_HOME + "\\runs\\" + run_dir.name
    plan = {"dir": pcrun, "duration": D + 4,
            "procs": [_pc_proc(n, c, d, pcrun) for d, n, c in spec["pc"]],
            "poll": spec.get("poll"), "resources_every": spec.get("resources_every", 0)}
    # The PC's test source first, confirmed by its status file, before anything else starts.
    test_sources = [cmd for m, cmd in spec["route"] if m == "pc" and "test_source" in cmd]
    if test_sources:
        set_pc_test_source(test_sources, notes)
    (run_dir / "pc_plan.json").write_text(json.dumps(plan, indent=1))
    beamer.ssh(f"New-Item -ItemType Directory -Force {pcrun} | Out-Null")
    beamer.scp_to(run_dir / "pc_plan.json", pcrun + "\\plan.json")
    clock = ClockLink(["ssh", beamer.PC, f"{PC_PY} -u {PC_HOME}\\clock.py serve"]).start()
    res = [{"t": time.monotonic(), "procs": mac.resources()}]
    agg = None
    if spec.get("agg"):
        agg = subprocess.Popen([str(AGG), AGG_UID, "E2E Unplug"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        notes.append({"t": time.monotonic(), "event": "agg_on", "out": agg.stdout.readline().decode().strip()})
        time.sleep(2.0)  # Beamer's list (300 ms debounce) and the flood
    t0 = time.monotonic()
    meta["t0"], meta["t_send_start"] = t0, t0 + T_ROUTE
    pcside = subprocess.Popen(["ssh", beamer.PC, f"{PC_PY} {PC_HOME}\\pcside.py {pcrun}\\plan.json"],
                              stdout=open(run_dir / "pcside.out", "w"), stderr=subprocess.STDOUT)
    procs = MacProcs(run_dir)
    timeline = [(d, "spawn", n, c) for d, n, c in spec["mac"]]
    # One command file per machine: the app reads the file once a second, so a second write before
    # that would replace the first (seen in WP10's first mic_mac_pc run: the test source was lost).
    # The PC's test source is already in force (set_pc_test_source above); the rest of its part goes
    # a little before the Mac's, as before.
    per_machine = {}
    for m, cmd in spec["route"]:
        if not (m == "pc" and "test_source" in cmd):
            per_machine.setdefault(m, []).append(cmd)
    timeline += [(T_ROUTE - (1.2 if m == "pc" else 0.0), "route", m, cmds) for m, cmds in sorted(per_machine.items())]
    timeline += [(t, "action", (m, what), arg) for t, m, what, arg in spec.get("actions", [])]
    timeline += [(D, "idle", None, None)]
    timeline.sort(key=lambda x: x[0])
    status_log = open(run_dir / "mac_status.jsonl", "w")
    redeploy = None
    next_status = t0
    next_res = t0 + (spec.get("resources_every") or 1e12)
    for at, kind, a, b in timeline:
        while time.monotonic() - t0 < at:
            now = time.monotonic()
            if now >= next_status:
                next_status = now + 1.0
                st = mac.status()
                if st:
                    snap = st["snapshot"]
                    status_log.write(json.dumps({"t": now, "mic": snap["mic_status"], "out": snap["output_status"],
                                                 "streams": snap["streams"], "helper": snap["helper"],
                                                 "perm": snap["permission"]}) + "\n")
            if now >= next_res:
                next_res = now + spec["resources_every"]
                res.append({"t": now, "procs": mac.resources()})
            time.sleep(0.05)
        now = time.monotonic()
        if kind == "spawn":
            procs.spawn(a, b)
            notes.append({"t": now, "event": "spawn", "name": a})
        elif kind == "route":
            (mac if a == "mac" else pc).command(b)
            notes.append({"t": now, "event": "route", "machine": a, "cmd": b})
        elif kind == "idle":
            st = mac.status()
            mac.command(beamer.idle_commands(st))
            pc.command([{"test_source": {"kind": "mic", "signal": None}}])
            notes.append({"t": now, "event": "idle"})
        elif kind == "action":
            machine, what = a
            note = {"t": now, "event": what, "machine": machine}
            if what == "command":
                (mac if machine == "mac" else pc).command(b)
                note["cmd"] = b
            elif what == "kill_helper":
                if machine == "pc":
                    pid = pc.helper_pid()
                    note["pid"] = pid
                    note["t"] = time.monotonic()
                    beamer.ssh(f"Stop-Process -Id {pid} -Force")
                    note["t_done"] = time.monotonic()
                else:
                    pid = mac.pids().get("beamer-audio")
                    note["pid"] = pid
                    note["t"] = time.monotonic()
                    os.kill(pid, 9)
            elif what == "agg_off":
                agg.stdin.close()
                note["out"] = agg.stdout.readline().decode().strip()
                agg.wait(timeout=5)
            elif what == "agg_on":
                agg = subprocess.Popen([str(AGG), AGG_UID, "E2E Unplug"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
                note["out"] = agg.stdout.readline().decode().strip()
            elif what == "redeploy":
                redeploy = threading.Thread(target=_redeploy, args=(run_dir, notes), daemon=True)
                redeploy.start()
            notes.append(note)
    meta["t_idle"] = time.monotonic()
    procs.wait(timeout=15)
    if redeploy is not None:
        redeploy.join(timeout=600)
    try:
        pcside.wait(timeout=90)
    except subprocess.TimeoutExpired:
        pcside.kill()
    if agg is not None and agg.poll() is None:
        agg.stdin.close()
        agg.wait(timeout=5)
    res.append({"t": time.monotonic(), "procs": mac.resources()})
    pings = clock.stop()
    meta["t_end"] = time.monotonic()
    (run_dir / "pings.json").write_text(json.dumps({"pings": pings}))
    (run_dir / "mac_resources.jsonl").write_text("".join(json.dumps(r) + "\n" for r in res))
    (run_dir / "notes.jsonl").write_text("".join(json.dumps(n) + "\n" for n in notes))
    (run_dir / "meta.json").write_text(json.dumps(meta, indent=1))
    mac.copy_events(run_dir / "mac_events.jsonl")
    (run_dir / "pc").mkdir(exist_ok=True)
    beamer.scp_from(pcrun + "\\*", run_dir / "pc")
    pc.copy_events(run_dir / "pc" / "pc_events.jsonl")


def _redeploy(run_dir, notes):
    """dev/deploy.sh mac under the deploy lock: Beamer quits (link down) and comes back (persist)."""
    t = time.monotonic()
    while True:
        try:
            DEPLOY_LOCK.mkdir()
            break
        except FileExistsError:
            time.sleep(5)
    try:
        notes.append({"t": time.monotonic(), "event": "deploy_start", "waited_s": round(time.monotonic() - t, 1)})
        r = subprocess.run([str(REPO / "dev" / "deploy.sh"), "mac"], cwd=REPO, capture_output=True, text=True)
        notes.append({"t": time.monotonic(), "event": "deploy_done", "rc": r.returncode, "out": r.stdout[-300:]})
    finally:
        DEPLOY_LOCK.rmdir()


def oldpeer(run_dir):
    """§11.4 oldpeer: tools/older_peer_check.py against Beamer 1.4.3's source (git tag v1.4.3), and the
    PC log since its last start: the PC<->asusrog link (a 1.5.x peer without `audio`) came up with no
    audio cap and nothing audio_* went to it."""
    src = run_dir / "beamer-1.4.3"
    src.mkdir(parents=True, exist_ok=True)
    subprocess.run(f"git -C {shlex.quote(str(REPO))} archive v1.4.3 | tar -x -C {shlex.quote(str(src))}", shell=True, check=True)
    # this tree's core needs PyNaCl, 1.4.3's needs cryptography
    check = subprocess.run(["uv", "run", "-q", "--python", "3.13", "--with", "pynacl", "--with", "cryptography",
                            "--with", "pyobjc-core", "--with", "pyobjc-framework-Cocoa", "--with", "pyobjc-framework-Quartz", "python",
                            str(REPO / "tools" / "older_peer_check.py"), str(src)], cwd=REPO, capture_output=True,
                           text=True, timeout=600)
    log = beamer.ssh(f"Select-String -Path {beamer.PC_DIR}\\Beamer.log -Pattern 'asusrog' | ForEach-Object {{ $_.Line }}",
                     check=False)
    lines = [ln for ln in log.splitlines() if ln.strip()]
    bad = [ln for ln in lines if "audio_" in ln or "WARNING" in ln or "ERROR" in ln]
    up = [ln for ln in lines if "out link with asusrog up" in ln]
    result = {"case": "oldpeer", "older_peer_check_rc": check.returncode, "older_peer_check_tail": check.stdout[-1500:],
              "asusrog_link_up_lines": up[-10:], "asusrog_audio_or_warnings": bad[-10:],
              # The tool's own verdict also wants acead88's texts (not in this repo); the audio criterion is
              # that nothing connects across versions and the 1.5.x peer link stays audio-free.
              "refused_both_ways": "[CONNECTED]" not in check.stdout and "v6" in check.stdout,
              "pass": "[CONNECTED]" not in check.stdout and bool(up) and not bad and all("no audio" in ln for ln in up)}
    (run_dir / "result.json").write_text(json.dumps(result, indent=1))
    print(json.dumps(result, indent=1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("case")
    ap.add_argument("--duration", type=float, default=None)
    ap.add_argument("--name", default=None)
    ap.add_argument("--no-analyze", action="store_true")
    a = ap.parse_args()
    if a.case == "setup":
        return setup()
    if a.case == "oldpeer":
        run_dir = RUNS / (a.name or f"oldpeer_{time.strftime('%m%d_%H%M%S')}")
        return oldpeer(run_dir)
    if a.case == "persist":
        a.case = "linkloss"  # §11.4 persist: the same run (route re-applied after both apps' restart)
    if a.case == "idle":
        for m in (mac, pc):
            m.command(beamer.idle_commands(m.status()))
        return print("idle sent")
    if not LAB_LOCK.exists():
        sys.exit("hold ~/Developer/.beamer-audio-lab.lock first (mkdir it)")
    D = a.duration or {"soak": 1860, "linkloss": 150}.get(a.case, 75)
    ctx = Ctx()
    spec = cases(ctx, D)[a.case]
    if spec.get("missing"):
        sys.exit(f"{a.case} needs {spec['missing']}")
    if not beamer.is_idle(ctx.mac_status):
        mac.command(beamer.idle_commands(ctx.mac_status))
        time.sleep(3)
    name = a.name or f"{a.case}_{time.strftime('%m%d_%H%M%S')}"
    run_dir = RUNS / name
    run_dir.mkdir(parents=True, exist_ok=True)
    notes = []
    print(f"run {name}: {spec['desc']} ({D:.0f} s)", flush=True)
    try:
        run_case(a.case, spec, D, run_dir, notes)
    finally:
        try:
            mac.command(beamer.idle_commands(mac.status()))
            pc.command([{"test_source": {"kind": "mic", "signal": None}}])
        except Exception as error:  # noqa: BLE001
            print("could not put the route back to idle:", error)
    if not a.no_analyze:
        subprocess.run([PY, str(HERE / "results.py"), str(run_dir)])


if __name__ == "__main__":
    main()
