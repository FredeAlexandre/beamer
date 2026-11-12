# SPDX-License-Identifier: GPL-3.0-only
"""Driving both Beamer apps for the camera E2E cases: dev command files, status files, one timed session
(Mac actions + the PC plan run by pcside_cam.py/pcagent.py), and collection of everything into a run folder.

Built on dev/audio/e2e (beamer.py: the dev command files over ssh/scp; clock.py: the ping clock link), which
are imported, never edited. The PC half lives in its own folder (BEAMER_CAM_E2E_PC_HOME, default
C:\\Users\\Alex\\camera-cw11), so the audio harness's folder is never touched.
"""
from __future__ import annotations

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
RUNS = HERE / "runs"

PC_HOME = os.environ.get("BEAMER_CAM_E2E_PC_HOME", r"C:\Users\Alex\camera-cw11")
# beamer.py stages the PC's dev command file in its PC_HOME: point it at ours before importing it.
os.environ["BEAMER_E2E_PC_HOME"] = PC_HOME
sys.path.insert(0, str(AUDIO_E2E))
import beamer  # noqa: E402  (dev/audio/e2e/beamer.py)
from clock import ClockLink  # noqa: E402  (dev/audio/e2e/clock.py)

PC = beamer.PC
PC_PY = PC_HOME + r"\venv\Scripts\python.exe"
PC_CLONE = r"C:\Users\Alex\Developer\beamer"
PC_PROBE = PC_HOME + r"\camprobe.exe"
PC_CABLE_TOOL = PC_HOME + r"\proto-audio.exe"
# CABLE Output's endpoint id on lebig (as dev/audio/e2e/run.py CABLE_OUT)
CABLE_OUT = "{0.0.1.00000000}.{ddc7e301-3260-4ed3-8389-0938254820e0}"
MAC_DIR = beamer.MAC_DIR
PC_DIR = beamer.PC_DIR
LAB_LOCK = Path.home() / "Developer" / ".beamer-camera-lab.lock"
AUDIO_LAB_LOCK = Path.home() / "Developer" / ".beamer-audio-lab.lock"
DEPLOY_LOCK = Path.home() / "Developer" / ".beamer-deploy.lock"

mac, pc = beamer.Mac(), beamer.Pc()


class Stop(Exception):
    """A precondition is missing: the run stops with this message (not a test failure)."""


# ---------------------------------------------------------------- small helpers

def ssh(command, check=True, timeout=60):
    return beamer.ssh(command, check=check, timeout=timeout)


def pc_reachable(timeout=5):
    r = subprocess.run(["ssh", "-o", f"ConnectTimeout={timeout}", "-o", "BatchMode=yes", PC, "exit"],
                       capture_output=True, text=True, timeout=timeout + 10)
    return r.returncode == 0, (r.stderr or "").strip()[:200]


def now():
    return time.monotonic()


def camera(status):
    """The camera part's dev status (`camera` key), or {}."""
    return (status or {}).get("camera") or {}


def cam_snap(status):
    return camera(status).get("snapshot") or {}


def wait_for(get, pred, timeout, period=0.3, what="condition"):
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        last = get()
        try:
            if last is not None and pred(last):
                return last
        except (KeyError, TypeError, AttributeError):
            pass
        time.sleep(period)
    raise Stop(f"timed out after {timeout:.0f} s waiting for {what}")


def lock():
    """The camera lab lock (mkdir is atomic). Held for a whole `run.py` invocation."""
    try:
        LAB_LOCK.mkdir()
    except FileExistsError:
        owner = (LAB_LOCK / "owner").read_text() if (LAB_LOCK / "owner").exists() else "?"
        raise Stop(f"{LAB_LOCK} is held ({owner.strip()}): another camera E2E run is going on. "
                   f"If not, remove it: rmdir {LAB_LOCK} (after deleting its 'owner' file)")
    (LAB_LOCK / "owner").write_text(f"pid {os.getpid()} since {time.strftime('%Y-%m-%d %H:%M:%S')}\n")


def unlock():
    try:
        (LAB_LOCK / "owner").unlink()
        LAB_LOCK.rmdir()
    except OSError:
        pass


# ---------------------------------------------------------------- the two machines' camera state

class Ctx:
    """Ids from both status files, refreshed on demand."""

    def __init__(self):
        self.refresh()

    def refresh(self):
        self.mac_status = mac.status()
        self.pc_status = pc.status()
        if not self.mac_status or not self.pc_status:
            raise Stop("a dev status file is missing (is Beamer running in dev mode on both machines?)")
        self.mac_id = self.mac_status["snapshot"]["own"]
        self.pc_id = self.pc_status["snapshot"]["own"]
        return self

    def mac_mic(self):
        return beamer.device(self.mac_status, self.mac_id, "inputs", "BuiltInMicrophoneDevice")


def set_test_source(value, timeout=15.0):
    """Mac synthetic source (`camera_test_source`), confirmed by the Mac status file (§7.4: the harness waits
    until audio-dev-status.json names it before setting the route)."""
    mac.command({"camera_test_source": value})
    wait_for(mac.status, lambda s: camera(s).get("test_source") == value, timeout,
             what=f"the Mac to report camera test_source {value!r} (needs a `--dev` helper with camera_synth)")


def mac_snapshot_ok(status):
    return bool(cam_snap(status))


def prepare(ctx, quality="720p", test_source="synth", floor_ms=50, sharing=True, route=True):
    """Common start of every synthetic case: sharing on, test source, cluster quality, route Mac camera ->
    (the PC), PC refusal off, playout floor; waits until both status files show it."""
    notes = []
    mac.command([{"camera_sharing": sharing}, {"set_quality": quality},
                 {"helper_dev": {"video_loss": {"pct": 0, "burst": 0}, "idr_pace_x": 4.0}}])
    pc.command([{"camera_refuse": None}, {"helper_dev": {"playout_floor_ms": floor_ms, "drop_udp": False}}])
    if test_source is not None:
        set_test_source(test_source)
    else:
        mac.command({"camera_test_source": None})
    if route:
        mac.command({"set_camera": {"machine": ctx.mac_id, "device": None}})
        wait_for(pc.status, lambda s: (cam_snap(s).get("route", {}).get("camera", {}).get("machine") == ctx.mac_id
                                       and cam_snap(s).get("route", {}).get("quality", {}).get("size") == quality),
                 30, period=1.0, what="the PC to hold the camera route (Mac camera, %s)" % quality)
        if sharing:
            # §6.4 rule 2 + §4.11: the Mac keeps a standby encoder ready once the route names it
            try:
                wait_for(mac.status, lambda s: camera(s).get("warm"), 10, what="the Mac's warm encoder plan")
                notes.append("warm encoder planned")
            except Stop:
                notes.append("no warm encoder in the Mac plan after 10 s")
    return notes


def teardown(ctx=None):
    """Route off, test source off, 720p, refusal off, dev switches back to defaults. Best effort."""
    errors = []
    try:
        mac.command([{"set_camera": {"machine": None, "device": None}}, {"camera_test_source": None},
                     {"set_quality": "720p"}, {"camera_sharing": True},
                     {"helper_dev": {"video_loss": {"pct": 0, "burst": 0}, "idr_pace_x": 4.0}},
                     {"set_mic": {"machine": None, "device": None}}, {"test_source": {"kind": "mic", "signal": None}}])
    except Exception as error:  # noqa: BLE001
        errors.append(f"mac: {error}")
    try:
        pc.command([{"camera_refuse": None}, {"helper_dev": {"playout_floor_ms": 50, "drop_udp": False}}])
    except Exception as error:  # noqa: BLE001
        errors.append(f"pc: {error}")
    return errors


# ---------------------------------------------------------------- resources on the Mac

def mac_resources():
    rows = mac.resources()
    out = subprocess.run(["pgrep", "-x", "VTEncoderXPCService"], capture_output=True, text=True).stdout.split()
    for pid in out:
        o = subprocess.run(["ps", "-o", "time=,rss=", "-p", pid], capture_output=True, text=True).stdout.split()
        if len(o) == 2:
            rows.append({"name": "VTEncoderXPCService", "pid": int(pid), "cpu": beamer._cputime(o[0]),
                         "rss": int(o[1]) * 1024})
    for r in rows:
        if r["name"] == "beamer-audio":
            r["name"] = "helper"
    return rows


# ---------------------------------------------------------------- events files

def read_events(path: Path, t0, t1):
    """Records of audio-dev-events.jsonl (and its rotated `.1`) with mono in [t0, t1] (that machine's clock)."""
    out = []
    for p in (path.with_name(path.name + ".1"), path):
        if not p.exists():
            continue
        with open(p, encoding="utf-8", errors="replace") as f:
            for line in f:
                if '"mono"' not in line:
                    continue
                try:
                    e = json.loads(line)
                except ValueError:
                    continue
                m = e.get("mono")
                if isinstance(m, (int, float)) and t0 <= m <= t1:
                    out.append(e)
    out.sort(key=lambda e: e["mono"])
    return out


# ---------------------------------------------------------------- one timed session

class Session:
    """One run: Mac actions on a timeline + the PC plan (reads, kills, cable) + samplers, then collection.

    actions: [(at_s, label, fn)] run on the Mac (fn() may write dev commands; PC commands go through ssh, so
    they run in a thread and their start/end are noted). pc_items: pcagent items. pc_status_every: poll the
    PC status file (ssh) every N s into pc_status.jsonl (0 = never)."""

    LEAD = 8.0   # s between planning and t=0: scp + ssh + scheduled task start (~2-3 s seen in C4/audio)

    def __init__(self, name, case, duration, actions=(), pc_items=(), kills=(), cable=False,
                 resources_every=0, pc_status_every=0, mac_status_every=1.0, meta=None):
        self.name, self.case, self.duration = name, case, duration
        self.actions = sorted(actions, key=lambda a: a[0])
        self.pc_items, self.kills, self.cable = list(pc_items), list(kills), cable
        self.resources_every, self.pc_status_every, self.mac_status_every = resources_every, pc_status_every, mac_status_every
        self.dir = RUNS / name
        self.dir.mkdir(parents=True, exist_ok=True)
        self.pc_dir = PC_HOME + "\\runs\\" + name.replace("/", "\\")
        self.notes = []
        self.meta = dict(meta or {}, name=name, case=case, duration=duration,
                         started=time.strftime("%Y-%m-%d %H:%M:%S"))

    def note(self, **kw):
        kw.setdefault("t", time.monotonic())
        self.notes.append(kw)
        return kw

    def run(self):
        clock = ClockLink(["ssh", PC, f"{PC_PY} -u {PC_HOME}\\clock.py serve"]).start()
        try:
            return self._run(clock)
        finally:
            pings = clock.stop()
            (self.dir / "pings.json").write_text(json.dumps({"pings": pings}))

    def _offset_now(self, clock):
        good = sorted(clock.pings, key=lambda p: p[1])[: max(1, len(clock.pings) // 3)]
        if not good:
            raise Stop("no clock pings from the PC (clock.py serve over ssh)")
        return sum(p[2] for p in good) / len(good)

    def _run(self, clock):
        t_wait = time.monotonic() + 15
        while len(clock.pings) < 8 and time.monotonic() < t_wait:
            time.sleep(0.25)
        off = self._offset_now(clock)
        t0 = time.monotonic() + self.LEAD
        plan = {"dir": self.pc_dir, "start_mono": t0 + off, "duration": self.duration, "items": self.pc_items,
                "kills": self.kills, "resources_every": self.resources_every, "cable": self.cable,
                "cable_cmd": f'"{PC_CABLE_TOOL}" capture-mic --device {CABLE_OUT} --channels 1 > NUL',
                "probe": PC_PROBE}
        (self.dir / "pc_plan.json").write_text(json.dumps(plan, indent=1))
        ssh(f"New-Item -ItemType Directory -Force {self.pc_dir} | Out-Null")
        beamer.scp_to(self.dir / "pc_plan.json", self.pc_dir + "\\plan.json")
        pcside = subprocess.Popen(["ssh", PC, f"{PC_PY} {PC_HOME}\\pcside_cam.py {self.pc_dir}\\plan.json"],
                                  stdout=open(self.dir / "pcside.out", "w"), stderr=subprocess.STDOUT)
        self.meta.update(t0=t0, offset_at_plan=off)
        mac_res = [{"t": time.monotonic(), "procs": mac_resources()}]
        mac_status_log = open(self.dir / "mac_status.jsonl", "w")
        pc_status_log = open(self.dir / "pc_status.jsonl", "w") if self.pc_status_every else None
        threads = []
        i = 0
        next_ms = next_ps = next_res = t0
        end = t0 + self.duration
        while time.monotonic() < end:
            tn = time.monotonic()
            if pcside.poll() is not None and not self.meta.get("pcside_rc_early"):
                self.meta["pcside_rc_early"] = pcside.returncode
                out = (self.dir / "pcside.out").read_text(errors="replace")[-400:]
                if pcside.returncode == 3 or "H8" in out:
                    raise Stop("H8: nobody is signed in on lebig (the PC app's camera reads need his session): "
                               "Alex signs in at the PC, then run again")
                raise Stop(f"the PC half (pcside_cam.py) ended early, rc {pcside.returncode}: {out.strip()}")
            while i < len(self.actions) and tn >= t0 + self.actions[i][0]:
                at, label, fn = self.actions[i]
                i += 1
                rec = self.note(event="action", at=at, label=label)

                def go(fn=fn, rec=rec):
                    try:
                        fn()
                    except Exception as error:  # noqa: BLE001
                        rec["error"] = repr(error)
                    rec["t_done"] = time.monotonic()
                th = threading.Thread(target=go, daemon=True)
                th.start()
                threads.append(th)
            if self.mac_status_every and tn >= next_ms:
                next_ms = tn + self.mac_status_every
                st = mac.status()
                if st:
                    mac_status_log.write(json.dumps({"t": tn, "camera": camera(st)}) + "\n")
            if pc_status_log and tn >= next_ps:
                next_ps = tn + self.pc_status_every
                threading.Thread(target=self._poll_pc, args=(pc_status_log,), daemon=True).start()
            if self.resources_every and tn >= next_res:
                next_res = tn + self.resources_every
                mac_res.append({"t": tn, "procs": mac_resources()})
            time.sleep(0.02)
        for th in threads:
            th.join(timeout=30)
        mac_res.append({"t": time.monotonic(), "procs": mac_resources()})
        self.meta["t_end"] = time.monotonic()
        try:
            pcside.wait(timeout=150)
        except subprocess.TimeoutExpired:
            pcside.kill()
            self.note(event="pcside_timeout")
        mac_status_log.close()
        if pc_status_log:
            time.sleep(1.0)
            pc_status_log.close()
        (self.dir / "mac_resources.jsonl").write_text("".join(json.dumps(r) + "\n" for r in mac_res))
        self.collect(t0 - 30, self.meta["t_end"] + 30, off)
        return self

    def _poll_pc(self, log):
        t = time.monotonic()
        try:
            st = pc.status()
            log.write(json.dumps({"t": t, "t_done": time.monotonic(), "camera": camera(st)}) + "\n")
            log.flush()
        except Exception as error:  # noqa: BLE001
            log.write(json.dumps({"t": t, "error": repr(error)}) + "\n")

    def collect(self, t0, t1, off):
        (self.dir / "notes.jsonl").write_text("".join(json.dumps(n) + "\n" for n in self.notes))
        (self.dir / "meta.json").write_text(json.dumps(self.meta, indent=1))
        ev = read_events(MAC_DIR / "audio-dev-events.jsonl", t0, t1)
        (self.dir / "mac_events.jsonl").write_text("".join(json.dumps(e) + "\n" for e in ev))
        # PC: copy the events files (current + rotated) into the run folder there, then everything back
        ssh(f"Copy-Item -Force {PC_DIR}\\audio-dev-events.jsonl {self.pc_dir}\\pc_events.jsonl -EA SilentlyContinue; "
            f"if (Test-Path {PC_DIR}\\audio-dev-events.jsonl.1) {{ Copy-Item -Force {PC_DIR}\\audio-dev-events.jsonl.1 "
            f"{self.pc_dir}\\pc_events.jsonl.1 }}", check=False)
        (self.dir / "pc").mkdir(exist_ok=True)
        beamer.scp_from(self.pc_dir + "\\*", self.dir / "pc")
        pc_ev = read_events(self.dir / "pc" / "pc_events.jsonl", t0 + off - 60, t1 + off + 60)
        (self.dir / "pc" / "pc_events.jsonl").write_text("".join(json.dumps(e) + "\n" for e in pc_ev))
        rotated = self.dir / "pc" / "pc_events.jsonl.1"
        if rotated.exists():
            rotated.unlink()


# ---------------------------------------------------------------- loading a run back (analysis side)

class Run:
    """A collected run folder, on the Mac time base (see analysis.py)."""

    def __init__(self, run_dir):
        import analysis as A
        self.A = A
        self.dir = Path(run_dir)
        self.meta = json.loads((self.dir / "meta.json").read_text())
        self.clock = A.Clock(json.loads((self.dir / "pings.json").read_text())["pings"])
        pcs = A.jl(self.dir / "pc" / "pcside.jsonl")
        pairs = [e for e in pcs if e.get("event") == "pair"]
        self.pcside = pcs
        self.pctime = A.PcTime(self.clock, pairs) if pairs else None
        self.t0, self.t_end = self.meta["t0"], self.meta.get("t_end", self.meta["t0"] + self.meta["duration"])
        self.mac_events = A.events_in(A.jl(self.dir / "mac_events.jsonl"), self.t0 - 30, self.t_end + 30)
        self.pc_events = A.events_in(A.jl(self.dir / "pc" / "pc_events.jsonl"), self.t0 - 30, self.t_end + 30,
                                     to_mac=self.clock.pc_to_mac)
        self.agent = [dict(e, t=self.clock.pc_to_mac(e["mono"])) for e in A.jl(self.dir / "pc" / "agent.jsonl")]
        self.notes = A.jl(self.dir / "notes.jsonl")
        self.mac_status = A.jl(self.dir / "mac_status.jsonl")
        self.pc_status = A.jl(self.dir / "pc_status.jsonl")

    def at(self, label):
        """Mac time at which the action `label` was started (first match)."""
        n = next((n for n in self.notes if n.get("event") == "action" and n.get("label") == label), None)
        return n["t"] if n else None

    def actions(self, prefix):
        return [n for n in self.notes if n.get("event") == "action" and str(n.get("label", "")).startswith(prefix)]

    def read(self, tag):
        """(Frames, summary, t_open, t_closed) of one app read, Mac time base."""
        A = self.A
        dump = self.dir / "pc" / f"{tag}.dump"
        out = self.dir / "pc" / f"{tag}.out"
        if not dump.exists() or self.pctime is None:
            return None
        frames = A.Frames.from_dump(A.parse_dump(dump.read_text(errors="replace")), self.pctime.unix_us, tag)
        summary, lines = A.parse_probe_out(out.read_text(errors="replace") if out.exists() else "")
        t_open = self.pctime.unix_us(summary["t_begin"]) if summary and summary.get("t_begin") else None
        t_closed = self.pctime.unix_us(summary["t_closed"]) if summary and summary.get("t_closed") else None
        if t_open is None:
            spawn = next((e for e in self.agent if e.get("ev") == "spawn" and e.get("tag") == tag), None)
            t_open = spawn["t"] if spawn else None
        return {"frames": frames, "summary": summary, "lines": lines, "t_open": t_open, "t_closed": t_closed}

    def pc_resources(self):
        A = self.A
        return [{"t": self.clock.pc_to_mac(r["mono"]), "procs": r.get("procs", [])}
                for r in A.jl(self.dir / "pc" / "resources.jsonl") if "mono" in r]

    def mac_resources(self):
        return self.A.jl(self.dir / "mac_resources.jsonl")

    def kills(self):
        return [dict(e, t=self.clock.pc_to_mac(e["mono"])) for e in self.pcside if e.get("event") == "kill_helper"]
