# SPDX-License-Identifier: GPL-3.0-only
"""Pre-flight for the camera E2E runs: both machines run the same build with the camera, in dev mode, linked,
and Alex's Windows session is signed in (H8). Stops with a clear message instead of failing cases.

    python run.py preflight [--real]

Blocking (exit 2, nothing runs): PC off/unreachable; PC clone behind the Mac's last product commit (deploy
missing); an installed app older than that commit; Beamer not running or not in dev mode on a machine; no
camera part / no video helper; no camera link between them; Beamer Camera not installed on the PC; nobody
signed in on lebig (H8); the deploy lock held. `--real`: also Beamer.app's Camera permission granted (H1).
Everything else is reported (Mac camera permission, which stays `undetermined` for the unattended cases).
"""
from __future__ import annotations

import subprocess
import time
from pathlib import Path

import camlib as L

PRODUCT_PATHS = ["core", "mac_app", "win_app", "native", "locales"]
MAC_APP = Path("/Applications/Beamer.app")


def git(*args):
    return subprocess.run(["git", "-C", str(L.REPO), *args], capture_output=True, text=True).stdout.strip()


def product_commit():
    """The newest commit that changes what gets built (the harness's own commits do not need a deploy)."""
    return git("log", "-1", "--format=%H %ct", "HEAD", "--", *PRODUCT_PATHS).split()


class Report:
    def __init__(self):
        self.rows = []

    def add(self, name, ok, detail, blocking=True):
        self.rows.append({"check": name, "ok": ok, "detail": detail, "blocking": blocking})
        mark = "ok  " if ok else ("STOP" if blocking else "warn")
        print(f"  [{mark}] {name}: {detail}", flush=True)
        return ok

    def blocked(self):
        return [r for r in self.rows if not r["ok"] and r["blocking"]]


def run(real=False, quiet_pc_off=False):
    """Returns (report, ctx_or_None). Raises L.Stop when the PC cannot be reached at all."""
    rep = Report()
    print("pre-flight", flush=True)
    head = git("rev-parse", "HEAD")
    branch = git("rev-parse", "--abbrev-ref", "HEAD")
    pc_commit, pc_time = product_commit()
    rep.add("mac clone", True, f"{branch} @ {head[:9]}; last product commit {pc_commit[:9]} "
            f"({time.strftime('%Y-%m-%d %H:%M', time.localtime(int(pc_time)))})", blocking=False)
    dirty = git("status", "--porcelain", "--untracked-files=no", "--", *PRODUCT_PATHS)
    rep.add("uncommitted product changes", not dirty,
            "none" if not dirty else f"{len(dirty.splitlines())} files (other agents' work; deploy only ships commits)",
            blocking=False)
    if DEPLOY_LOCK_HELD():
        rep.add("deploy lock", False, f"{L.DEPLOY_LOCK} is held: a deploy is running, wait for it")

    # ---- this Mac (checked first, so its state shows even when the PC is off)
    mac_bin = MAC_APP / "Contents" / "MacOS" / "Beamer"
    mac_helper = MAC_APP / "Contents" / "MacOS" / "beamer-audio"
    if mac_bin.exists():
        mt = min(mac_bin.stat().st_mtime, mac_helper.stat().st_mtime if mac_helper.exists() else 1e18)
        rep.add("mac app installed after the product commit", mt >= int(pc_time),
                f"{MAC_APP} built {time.strftime('%Y-%m-%d %H:%M', time.localtime(mt))}"
                + ("" if mt >= int(pc_time) else ": older, run dev/deploy.sh"))
    else:
        rep.add("mac app installed", False, f"{MAC_APP} missing: run dev/deploy.sh")
    pids = L.mac.pids()
    rep.add("mac Beamer + helper running", "Beamer" in pids and "beamer-audio" in pids, str(pids))
    rep.add("mac dev mode", (L.MAC_DIR / "audio-dev.enabled").exists(), str(L.MAC_DIR / "audio-dev.enabled"))
    sp = L.MAC_DIR / "audio-dev-status.json"
    mage = time.time() - sp.stat().st_mtime if sp.exists() else -1
    rep.add("mac dev status fresh", 0 <= mage <= 5, f"audio-dev-status.json {mage:.0f} s old")

    mst = L.mac.status()
    mcam = L.camera(mst)
    rep.add("mac camera part", bool(mcam), "`camera` in the dev status" if mcam else
            "no `camera` key in the Mac's dev status: the installed build has no camera (deploy)")
    if mst:
        feats = set(mst["snapshot"]["machines"].get(mst["snapshot"]["own"], {}).get("features", []))
        want = {"video1", "camera_capture", "camera_synth"}
        rep.add("mac helper features", want <= feats, f"has {sorted(feats & want)}, needs {sorted(want)}"
                + ("" if "camera_synth" in feats else " (camera_synth = helper started with --dev)"))

    ok, err = L.pc_reachable()
    if not ok:
        raise L.Stop(f"the PC ({L.PC}) does not answer over ssh ({err or 'timeout'}): it is off, asleep or off the "
                     f"tailnet. Switch it on (and sign in: H8), then run `dev/deploy.sh` and this again.")
    rep.add("pc reachable", True, f"ssh {L.PC}")

    # ---- same build on both
    out = L.ssh(f"Set-Location {L.PC_CLONE}; git rev-parse HEAD; git rev-parse --abbrev-ref HEAD", check=False).split()
    pc_head = out[0] if out else ""
    known = subprocess.run(["git", "-C", str(L.REPO), "cat-file", "-e", pc_head + "^{commit}"],
                           capture_output=True).returncode == 0 if pc_head else False
    contains = known and subprocess.run(["git", "-C", str(L.REPO), "merge-base", "--is-ancestor", pc_commit, pc_head],
                                        capture_output=True).returncode == 0
    rep.add("pc clone has the product commit", contains,
            f"PC clone at {pc_head[:9] or '?'} ({out[1] if len(out) > 1 else '?'})"
            + ("" if contains else f"; needs {pc_commit[:9]}: run dev/deploy.sh"))

    pc_info = L.ssh(
        "$i = Join-Path $env:LOCALAPPDATA 'Beamer'; "
        "$e = Get-Item (Join-Path $i 'Beamer.exe') -EA SilentlyContinue; "
        "$h = Join-Path $i 'beamer-audio.exe'; $b = Join-Path '%s' 'native\\audio\\build\\win\\beamer-audio.exe'; "
        "$hi = if (Test-Path $h) { (Get-FileHash $h).Hash } else { '' }; "
        "$hb = if (Test-Path $b) { (Get-FileHash $b).Hash } else { '' }; "
        "'exe=' + $(if ($e) { ([DateTimeOffset]$e.LastWriteTimeUtc).ToUnixTimeSeconds() } else { 0 }); "
        "'helper_same=' + ($hi -ne '' -and $hi -eq $hb); "
        "'probe=' + (Test-Path '%s\\native\\audio\\build\\win\\camprobe.exe'); "
        "'devmode=' + (Test-Path (Join-Path $i 'audio-dev.enabled')); "
        "$s = Get-Item (Join-Path $i 'audio-dev-status.json') -EA SilentlyContinue; "
        "'status_age=' + $(if ($s) { [int]((Get-Date) - $s.LastWriteTime).TotalSeconds } else { -1 }); "
        "'user=' + (Get-CimInstance Win32_ComputerSystem).UserName; "
        "'beamer=' + ((Get-Process Beamer -EA SilentlyContinue | ForEach-Object { $_.SessionId }) -join ','); "
        "'helper=' + ((Get-CimInstance Win32_Process -Filter \"Name='beamer-audio.exe'\" | "
        "Where-Object { $_.CommandLine -like '*--control*' } | ForEach-Object { $_.ProcessId }) -join ',')"
        % (L.PC_CLONE, L.PC_CLONE), check=False)
    info = dict(line.split("=", 1) for line in pc_info.splitlines() if "=" in line)
    exe_t = int(info.get("exe", "0") or 0)
    rep.add("pc app installed after the product commit", exe_t >= int(pc_time),
            f"Beamer.exe written {time.strftime('%Y-%m-%d %H:%M', time.localtime(exe_t)) if exe_t else 'never'}"
            + ("" if exe_t >= int(pc_time) else ": older or missing, run dev/deploy.sh"))
    rep.add("pc installed helper = clone build", info.get("helper_same") == "True",
            "same SHA-256" if info.get("helper_same") == "True" else "differs or missing (deploy not finished?)")
    rep.add("H8: Alex signed in on lebig", bool(info.get("user")),
            info.get("user") or "nobody signed in: sign in at the PC (the camera reads run in that session)")
    sessions = [s for s in info.get("beamer", "").split(",") if s]
    rep.add("pc Beamer running in a desktop session", any(s != "0" for s in sessions),
            f"sessions {sessions or 'none'}" + ("" if sessions else ": start it (scheduled task Beamer-lebig-Alex)"))
    rep.add("pc helper running", bool(info.get("helper")), f"pid {info.get('helper') or 'none'}")
    rep.add("pc dev mode", info.get("devmode") == "True",
            "audio-dev.enabled present" if info.get("devmode") == "True"
            else "create %LOCALAPPDATA%\\Beamer\\audio-dev.enabled ({}) and restart Beamer")
    age = int(info.get("status_age", "-1") or -1)
    rep.add("pc dev status fresh", 0 <= age <= 5, f"audio-dev-status.json {age} s old")
    rep.add("camprobe built on the PC", info.get("probe") == "True",
            "native\\audio\\build\\win\\camprobe.exe" if info.get("probe") == "True" else "missing (the deploy builds it)")

    # ---- the camera feature end to end
    ctx = None
    try:
        ctx = L.Ctx()
    except L.Stop as error:
        rep.add("status files", False, str(error))
        return rep, None
    for side, st, own in (("pc", ctx.pc_status, ctx.pc_id),):
        cam = L.camera(st)
        rep.add(f"{side} camera part", bool(cam), "`camera` in the dev status" if cam else "no `camera` key: build without the camera")
        feats = set(st["snapshot"]["machines"].get(own, {}).get("features", []))
        want = {"video1", "camera_capture", "camera_synth"} if side == "mac" else {"video1", "virtual_camera"}
        rep.add(f"{side} helper features", want <= feats, f"has {sorted(feats & (want | {'video1'}))}, needs {sorted(want)}"
                + ("" if want <= feats or side != "mac" or "camera_synth" in feats else " (camera_synth = helper started with --dev)"))
    msnap, psnap = L.cam_snap(ctx.mac_status), L.cam_snap(ctx.pc_status)
    rep.add("camera link mac -> pc", ctx.pc_id in msnap.get("machines", {}),
            "the Mac holds the PC's camera_devices" if ctx.pc_id in msnap.get("machines", {}) else
            "no camera_devices from the PC (link down, or no camera1 on one side)")
    rep.add("camera link pc -> mac", ctx.mac_id in psnap.get("machines", {}),
            "the PC holds the Mac's camera_devices" if ctx.mac_id in psnap.get("machines", {}) else "no camera_devices from the Mac")
    vcam = (psnap.get("machines", {}).get(ctx.pc_id, {}) or {}).get("virtual_camera", {})
    rep.add("Beamer Camera installed on the PC", vcam.get("install") == "ok" and vcam.get("present"),
            f"install {vcam.get('install')}, present {vcam.get('present')}"
            + ("" if vcam.get("install") == "ok" else " (install: the PC page's \"Install Beamer Camera...\" (UAC, H6), or over ssh "
                   "the helper's vcam-install, native/audio/vcam-win/README.md \"Dev install\")"))
    rep.add("no app holds Beamer Camera now", not vcam.get("in_use"),
            "free" if not vcam.get("in_use") else f"in use by {vcam.get('app') or 'an app'}: close it first")
    perm = msnap.get("permission")
    if real:
        rep.add("H1: Beamer.app has the Mac camera permission", perm == "granted",
                f"permission {perm}" + ("" if perm == "granted" else ": Alex grants Camera to Beamer on the Mac (H1)"))
    else:
        rep.add("mac camera permission (unattended cases need none)", True,
                f"{perm} (acceptance wants it still undetermined)", blocking=False)
    sharing = msnap.get("sharing")
    rep.add("mac camera sharing", True, f"{sharing} (the harness turns it on)", blocking=False)
    route = msnap.get("route", {}).get("camera", {}).get("machine")
    audio_route = ctx.mac_status["snapshot"].get("route", {})
    mic_busy = audio_route.get("mic", {}).get("machine") is not None
    rep.add("routes idle", not route and not mic_busy,
            f"camera route {route or 'off'}, mic {'routed' if mic_busy else 'off'}"
            + (" (an audio E2E running? the harness will change these)" if (route or mic_busy) else ""), blocking=False)
    lip = (L.HERE / "cam_lipsync.py").exists()
    rep.add("cam_lipsync.py (CW10)", True, "present" if lip else "not there yet: `all` skips it", blocking=False)
    return rep, ctx


def DEPLOY_LOCK_HELD():
    return L.DEPLOY_LOCK.exists()


if __name__ == "__main__":
    import sys
    try:
        r, _ = run(real="--real" in sys.argv)
    except L.Stop as e:
        print("STOP:", e)
        sys.exit(2)
    sys.exit(2 if r.blocked() else 0)
