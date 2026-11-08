# SPDX-License-Identifier: GPL-3.0-only
"""Both Beamer apps, driven through their dev command files (PHASE2 §8.1): no socket, no listener.

Mac: files in ~/Library/Application Support/Beamer. PC (`ssh lebig`): %LOCALAPPDATA%\\Beamer, written by
scp into the harness folder, then moved into place (one rename, so the app never reads half a file).
Each app reads and deletes `audio-dev-cmd.json` once a second and writes `audio-dev-status.json`; in
dev mode it also appends its helper's events to `audio-dev-events.jsonl` (stamped `mono` = the app's
time.monotonic()).
"""
from __future__ import annotations

import json
import os
import subprocess
import tempfile
import time
from pathlib import Path

PC = os.environ.get("BEAMER_PC", "lebig")
MAC_DIR = Path.home() / "Library" / "Application Support" / "Beamer"
PC_HOME = os.environ.get("BEAMER_E2E_PC_HOME", r"C:\Users\Alex\audio-wp10")
PC_DIR = r"C:\Users\Alex\AppData\Local\Beamer"
PC_PY = PC_HOME + r"\venv\Scripts\python.exe"


def ssh(command, check=True, timeout=60):
    r = subprocess.run(["ssh", PC, command], capture_output=True, text=True, timeout=timeout)
    if check and r.returncode != 0:
        raise RuntimeError(f"ssh {command!r}: {r.returncode} {r.stderr.strip()[:300]}")
    return r.stdout


def scp_to(local, remote_win_path):
    subprocess.run(["scp", "-q", str(local), f"{PC}:{remote_win_path.replace(chr(92), '/')}"], check=True)


def scp_from(remote_win_path, local):
    return subprocess.run(["scp", "-q", "-r", f"{PC}:{remote_win_path.replace(chr(92), '/')}", str(local)]).returncode


class Mac:
    name = "mac"

    def command(self, cmd):
        end = time.monotonic() + 3.0  # never replace a command the app has not read yet
        while (MAC_DIR / "audio-dev-cmd.json").exists() and time.monotonic() < end:
            time.sleep(0.1)
        tmp = MAC_DIR / ".audio-dev-cmd.e2e.tmp"
        tmp.write_text(json.dumps(cmd), encoding="utf-8")
        os.replace(tmp, MAC_DIR / "audio-dev-cmd.json")

    def status(self):
        for _ in range(5):
            try:
                return json.loads((MAC_DIR / "audio-dev-status.json").read_text(encoding="utf-8"))
            except (OSError, ValueError):
                time.sleep(0.2)
        return None

    def events_path(self):
        return MAC_DIR / "audio-dev-events.jsonl"

    def copy_events(self, dest):
        dest.write_bytes(self.events_path().read_bytes())

    def pids(self):
        out = subprocess.run(["pgrep", "-f", "Beamer.app/Contents/MacOS/"], capture_output=True, text=True).stdout
        pids = {}
        for pid in out.split():
            cmd = subprocess.run(["ps", "-o", "command=", "-p", pid], capture_output=True, text=True).stdout
            if "beamer-audio" in cmd:
                pids["beamer-audio"] = int(pid)
            elif cmd.strip().endswith("MacOS/Beamer"):
                pids["Beamer"] = int(pid)
        return pids

    def resources(self):
        """[{name, pid, cpu (s), rss (bytes)}] of Beamer and its helper."""
        rows = []
        for name, pid in self.pids().items():
            out = subprocess.run(["ps", "-o", "time=,rss=", "-p", str(pid)], capture_output=True, text=True).stdout.split()
            if len(out) == 2:
                rows.append({"name": name, "pid": pid, "cpu": _cputime(out[0]), "rss": int(out[1]) * 1024})
        return rows


def _cputime(text):
    """ps TIME ([[dd-]hh:]mm:ss.cc) -> seconds."""
    days = 0
    if "-" in text:
        d, text = text.split("-", 1)
        days = int(d)
    parts = [float(p) for p in text.split(":")]
    secs = 0.0
    for p in parts:
        secs = secs * 60 + p
    return days * 86400 + secs


class Pc:
    name = "pc"

    def command(self, cmd):
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8") as f:
            json.dump(cmd, f)
        try:
            scp_to(f.name, PC_HOME + r"\cmd.json")
        finally:
            os.unlink(f.name)
        ssh(f"$e = (Get-Date).AddSeconds(3); while ((Test-Path {PC_DIR}\\audio-dev-cmd.json) -and (Get-Date) -lt $e) "
            f"{{ Start-Sleep -Milliseconds 100 }}; Move-Item -Force {PC_HOME}\\cmd.json {PC_DIR}\\audio-dev-cmd.json")

    def status(self):
        for _ in range(5):
            try:
                return json.loads(ssh(f"Get-Content -Raw {PC_DIR}\\audio-dev-status.json"))
            except (ValueError, RuntimeError):
                time.sleep(0.3)
        return None

    def copy_events(self, dest):
        ssh(f"Copy-Item -Force {PC_DIR}\\audio-dev-events.jsonl {PC_HOME}\\events.copy.jsonl")
        scp_from(PC_HOME + r"\events.copy.jsonl", dest)

    def helper_pid(self):
        # the helper itself, not its mute guard (`beamer-audio.exe _guard`, same image)
        out = ssh("(Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'beamer-audio.exe' -and "
                  "$_.CommandLine -like '*--control*' -and $_.ExecutablePath -like '*\\Beamer\\beamer-audio.exe' }).ProcessId",
                  check=False).split()
        return int(out[0]) if out else None


def ids(status):
    """(own id, {name: id}, {id: platform}) from a status file."""
    snap = status["snapshot"]
    names = {m["name"]: ident for ident, m in snap["machines"].items()}
    plats = {ident: m["platform"] for ident, m in snap["machines"].items()}
    return snap["own"], names, plats


def device(status, machine, kind, contains):
    """DeviceRef {key, name} of the first device of `machine` whose name contains `contains`."""
    m = status["snapshot"]["machines"][machine]
    for d in m[kind]:
        if contains.lower() in d["name"].lower() or contains == d["key"]:
            return {"key": d["key"], "name": d["name"]}
    raise KeyError(f"no {kind} matching {contains!r} on {m['name']}: {[d['name'] for d in m[kind]]}")


def idle_commands(status):
    """Mic off, every output local, no test source."""
    snap = status["snapshot"]
    cmds = [{"set_mic": {"machine": None, "device": None}}, {"test_source": {"kind": "mic", "signal": None}}]
    for ident in snap["machines"]:
        out = snap["route"]["outputs"].get(ident)
        if out and out.get("to"):
            cmds.append({"set_output": {"machine": ident, "to": None, "device": None}})
    return cmds


def is_idle(status):
    snap = status["snapshot"]
    route = snap["route"]
    return (route["mic"].get("machine") is None and all(not o.get("to") for o in route["outputs"].values())
            and not snap["streams"])
