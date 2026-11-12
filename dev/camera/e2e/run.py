# SPDX-License-Identifier: GPL-3.0-only
"""Two-machine E2E runs of Beamer's camera (dev/camera/PHASE2.md §10.5), from this Mac. Single entry point.

    uv run --python 3.13 --with numpy dev/camera/e2e/run.py COMMAND [options]

Commands
  list                     the cases, their duration and what they need
  preflight [--real]       same commit + camera on both machines, dev mode, link, H8 (Alex signed in on lebig)
  setup                    the PC half in C:\\Users\\Alex\\camera-cw11 (camprobe from the deployed build, scripts, venv)
  CASE [options]           one case (see `list`); writes runs/CASE_<stamp>/result.json
  all [--with-soak]        pre-flight, setup, every unattended case in order (~1 h 35), then the table
  soak                     cam_soak (34 min)
  smoke                    3 opens + 60 s latency at 720p (~3 min): checks the harness end to end first
  real                     the real-camera cases (need H1: Beamer.app granted Camera; Alex present)
  analyze RUN_DIR          re-run the analysis of a collected case folder
  table                    markdown table of the newest result of every case -> runs/SUMMARY.md
  idle                     camera route off, test sources off, dev switches back to defaults on both
  replay-c4                dry run of the analysis on C4's recordings (dev/camera/proto/e2e/out/raw), Mac only
  selftest                 unit tests of the analysis (synthetic data), Mac only

Options: --dry-run (print the timelines, touch nothing), --cycles N, --seconds S, --qualities 720p,1080p,
         --parts a,b,c, --reps N, --drops N, --no-preflight, --keep-going, --yes (real cases without the prompt).

Both apps must run the same deployed commit (`dev/deploy.sh`) in dev mode (`audio-dev.enabled`) with real
helpers; the cases use the Mac's synthetic source (`camera_test_source`), so no camera permission is needed.
Routes change only through the dev command files. The camera lab lock (~/Developer/.beamer-camera-lab.lock)
is taken for the whole invocation; do not run the audio E2E at the same time.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import analysis as A  # noqa: E402
import cases as C  # noqa: E402

DRY_ONLY = {"list", "replay-c4", "selftest", "table", "analyze"}


def opts_of(a):
    o = {}
    if a.cycles:
        o["cycles"] = a.cycles
    if a.seconds:
        o["seconds"] = a.seconds
    if a.qualities:
        o["qualities"] = tuple(a.qualities.split(","))
    if a.parts:
        o["parts"] = tuple(a.parts.split(","))
    if a.reps:
        o["reps"] = a.reps
    if a.drops:
        o["drops"] = a.drops
    return o


def plan_text(name, spec):
    lines = [f"  [{spec['sub']}] {spec['desc']}",
             f"    duration {spec['duration']:.0f} s (+ ~25 s prepare/collect); prepare {spec['prepare']}"]
    ev = [(at, "mac" if cmd[0] == "mac" else "pc ", f"{label}: {json.dumps(cmd[1])}") for at, label, cmd in spec["actions"]]
    ev += [(i["at"], "pc ", f"app {i['kind']} '{i['tag']}' {i.get('seconds')} s {i.get('size') or ''}".rstrip())
           for i in spec["items"]]
    ev += [(k["at"], "pc ", "kill the helper (taskkill /F)") for k in spec["kills"]]
    for at, who, what in sorted(ev, key=lambda x: x[0]):
        lines.append(f"    t={at:7.1f}  {who}  {what}")
    extras = []
    if spec["cable"]:
        extras.append("CABLE Output held open by the PC (Mac mic streams on demand)")
    if spec["resources_every"]:
        extras.append(f"CPU/memory every {spec['resources_every']} s on both")
    if spec["pc_status_every"]:
        extras.append(f"PC status polled every {spec['pc_status_every']} s")
    if extras:
        lines.append("    + " + "; ".join(extras))
    return "\n".join(lines)


def minutes_of(specs):
    return sum(s["duration"] + 25 for s in specs) / 60


# ---------------------------------------------------------------- running

def run_case(name, o, ctx, dry=False, assume_yes=False):
    import camlib as L
    builder, analyzer, mins, needs = C.CASES[name]
    ids = C.Ids() if dry else C.Ids(ctx.mac_id, ctx.pc_id, ctx.mac_mic())
    specs = builder(ids, o)
    if dry:
        print(f"{name}  (~{minutes_of(specs):.1f} min; needs: {needs})")
        for spec in specs:
            print(plan_text(name, spec))
        return None
    stamp = time.strftime("%m%d_%H%M%S")
    case_dir = f"{name}_{stamp}"
    head = subprocess.run(["git", "-C", str(L.REPO), "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    print(f"== {name} -> runs/{case_dir} (~{minutes_of(specs):.1f} min)", flush=True)
    done = []
    try:
        for spec in specs:
            if spec.get("interactive") and not assume_yes:
                print(f"\n{spec['desc']}\nPress Enter when Alex is ready (the recording starts {L.Session.LEAD:.0f} s later).")
                input()
            notes = L.prepare(ctx, **spec["prepare"]) if spec["prepare"] is not None else []
            actions = [(at, label, _fn(L, cmd)) for at, label, cmd in spec["actions"]]
            meta = {"spec": spec, "opts": o, "prepare_notes": notes, "commit": head, "case_dir": case_dir}
            s = L.Session(f"{case_dir}/{spec['sub']}", name, spec["duration"], actions=actions, pc_items=spec["items"],
                          kills=spec["kills"], cable=spec["cable"], resources_every=spec["resources_every"],
                          pc_status_every=spec["pc_status_every"], meta=meta)
            print(f"   {spec['sub']}: {spec['desc']} ({spec['duration']:.0f} s)", flush=True)
            s.run()
            done.append(spec["sub"])
    finally:
        errors = L.teardown()
        if errors:
            print("   teardown:", errors)
    return analyze_dir(L.RUNS / case_dir)


_LOCKS = {}


def _fn(L, cmd):
    """A timed dev command. Actions run in threads (a PC command is an scp + ssh round trip); one lock per
    machine, because each writes through a single staging file and waits for the app to consume the last one."""
    import threading
    who, payload = cmd
    target = L.mac if who == "mac" else L.pc
    lock = _LOCKS.setdefault(who, threading.Lock())

    def go():
        with lock:
            target.command(payload)
    return go


def analyze_dir(case_dir):
    import camlib as L
    case_dir = Path(case_dir)
    subs = sorted(p for p in case_dir.iterdir() if p.is_dir() and (p / "meta.json").exists())
    if not subs:
        print(f"   {case_dir}: nothing collected")
        return None
    metas = [json.loads((p / "meta.json").read_text()) for p in subs]
    order = {m["spec"]["sub"]: i for i, m in enumerate(metas)}
    subs = sorted(subs, key=lambda p: order.get(p.name, 0))
    metas = [json.loads((p / "meta.json").read_text()) for p in subs]
    name = metas[0]["case"]
    analyzer = C.CASES[name][1]
    runs = [L.Run(p) for p in subs]
    specs = [m["spec"] for m in metas]
    try:
        checks, info = analyzer(runs, specs, metas[0].get("opts") or {})
    except Exception as error:  # noqa: BLE001  (a partial run must still leave a result)
        import traceback
        checks, info = [A.check("analysis", repr(error), False, "no exception")], {"traceback": traceback.format_exc()}
    result = {"case": name, "verdict": A.verdict(checks), "commit": metas[0].get("commit"), "started": metas[0].get("started"),
              "sessions": [p.name for p in subs], "clock": [r.clock.info for r in runs], "checks": checks, "info": info}
    (case_dir / "result.json").write_text(json.dumps(result, indent=1, default=str))
    print_result(result)
    return result


def print_result(result):
    print(f"   {result['case']}: {result['verdict'].upper()}")
    for c in result["checks"]:
        mark = {True: "pass", False: "FAIL", None: " ?? "}[c["pass"]]
        val = json.dumps(c["value"], default=str)
        val = val if len(val) < 110 else val[:107] + "..."
        print(f"     [{mark}] {c['name']}: {val}  (mark {c['mark']}{'; ' + c['note'] if c.get('note') else ''})")


def oldpeer(ctx, dry=False):
    """PC <-> asusrog (a 1.5.x peer without `audio`): link unaffected, nothing camera_* or time_map sent."""
    import camlib as L
    if dry:
        print("cam_oldpeer  (~0.5 min): scans the PC's Beamer.log since its last start for asusrog lines")
        return None
    log = L.ssh(f"$l = Get-Content {L.PC_DIR}\\Beamer.log; $s = ($l | Select-String -Pattern 'Beamer [0-9.]+ start|starting Beamer' "
                f"| Select-Object -Last 1).LineNumber; if (-not $s) {{ $s = 1 }}; $l[($s-1)..($l.Count-1)] | "
                f"Select-String -Pattern 'asusrog' | ForEach-Object {{ $_.Line }}", check=False)
    lines = [x for x in log.splitlines() if x.strip()]
    up = [x for x in lines if "link" in x and "up" in x]
    cam = [x for x in lines if "camera" in x.lower() or "time_map" in x]
    warn = [x for x in lines if "WARNING" in x or "ERROR" in x]
    checks = [A.check("asusrog link up", up[-3:] or None, bool(up) or None, "link lines present",
                      "None = asusrog offline today: run again when it is on"),
              A.check("no camera_* / time_map towards asusrog (PC log)", cam[-5:], not cam, "0 lines"),
              A.check("no warnings about asusrog", warn[-5:], not warn, "0 lines")]
    result = {"case": "cam_oldpeer", "verdict": A.verdict(checks), "checks": checks,
              "info": {"lines": len(lines), "note": "asusrog's own n_rejected needs a shell on asusrog (not reachable from here)"}}
    d = L.RUNS / f"cam_oldpeer_{time.strftime('%m%d_%H%M%S')}"
    d.mkdir(parents=True, exist_ok=True)
    (d / "result.json").write_text(json.dumps(result, indent=1))
    print_result(result)
    return result


def lipsync(extra, dry=False, real=False):
    """CW10 owns cam_lipsync.py (`setup`, then `run [--modes shared,lowlat] [--seconds 300]`; it takes the camera lab
    lock itself, so this runner lets go of it around the call). Its result: runs/cam_lipsync/<name>/result.json."""
    f = HERE / "cam_lipsync.py"
    if real:
        print("cam_lipsync_real (CW10): the clap with the real camera and mic, in the session with Alex; "
              "cam_lipsync.py has no unattended variant for it")
        return {"case": "cam_lipsync_real", "verdict": "skipped", "checks": []}
    if not f.exists():
        print("cam_lipsync: dev/camera/e2e/cam_lipsync.py not there yet (CW10): skipped")
        return {"case": "cam_lipsync", "verdict": "skipped", "checks": []}
    cmds = [[sys.executable, str(f), "setup"], [sys.executable, str(f), "run"] + list(extra)]
    if dry:
        print("cam_lipsync (CW10, ~12 min with 2 x 300 s): would run\n    " + "\n    ".join(" ".join(c) for c in cmds))
        return None
    import camlib as L
    print("== cam_lipsync (CW10):", " ; ".join(" ".join(c[1:]) for c in cmds), flush=True)
    L.unlock()
    try:
        rc = 0
        for c in cmds:
            rc = subprocess.run(c, cwd=HERE).returncode
            if rc != 0:
                break
    finally:
        L.lock()
    res = lipsync_result()
    if res is None:
        return {"case": "cam_lipsync", "verdict": f"rc {rc}", "checks": []}
    return res


def lipsync_result():
    """CW10's newest result.json in this runner's shape (verdict + checks)."""
    found = sorted((HERE / "runs" / "cam_lipsync").glob("*/result.json"), key=lambda p: p.stat().st_mtime)
    if not found:
        return None
    r = json.loads(found[-1].read_text())
    checks = [A.check(f"A/V offset, {mode}", (m.get("offset_ms") or {}).get("p50"), m.get("pass"), r.get("mark", ""))
              for mode, m in (r.get("modes") or {}).items()]
    return {"case": "cam_lipsync", "verdict": "pass" if r.get("pass") else "fail", "checks": checks,
            "dir": f"cam_lipsync/{found[-1].parent.name}"}


def setup():
    import camlib as L
    print("setup: PC half in", L.PC_HOME, flush=True)
    out = L.ssh(
        f"New-Item -ItemType Directory -Force {L.PC_HOME}\\runs | Out-Null; Set-Location {L.PC_HOME}; "
        f"if (-not (Test-Path venv\\Scripts\\pythonw.exe)) {{ uv venv -q --python 3.13 venv 2>&1 | Out-Null; "
        f"  if (-not (Test-Path venv\\Scripts\\pythonw.exe)) {{ py -3.13 -m venv venv }} }}; "
        f"$p = '{L.PC_CLONE}\\native\\audio\\build\\win\\camprobe.exe'; "
        f"if (Test-Path $p) {{ Copy-Item -Force $p {L.PC_PROBE}; 'probe=ok' }} else {{ 'probe=missing' }}; "
        f"if (-not (Test-Path {L.PC_CABLE_TOOL})) {{ foreach ($c in 'C:\\Users\\Alex\\audio-wp16\\proto-audio.exe', "
        f"'C:\\Users\\Alex\\audio-wp10\\proto-audio.exe', 'C:\\Users\\Alex\\audio-proto\\scenarios\\beamer-audio.exe') {{ "
        f"if (Test-Path $c) {{ Copy-Item $c {L.PC_CABLE_TOOL}; break }} }} }}; "
        f"'cable=' + (Test-Path {L.PC_CABLE_TOOL}); "
        f"'venv=' + (Test-Path venv\\Scripts\\pythonw.exe); "
        f"'ffmpeg=' + [string](Get-Command ffmpeg -EA SilentlyContinue).Source", check=False, timeout=300)
    for f in (HERE / "pcside_cam.py", HERE / "pcagent.py", L.AUDIO_E2E / "clock.py"):
        L.beamer.scp_to(f, L.PC_HOME + "\\" + f.name)
    info = dict(x.split("=", 1) for x in out.splitlines() if "=" in x)
    print("  ", info)
    problems = []
    if info.get("probe") != "ok":
        problems.append("camprobe.exe is not in the PC clone's build (deploy first)")
    if info.get("venv") != "True":
        problems.append("no Python venv on the PC (uv / py -3.13 missing?)")
    if info.get("cable") != "True":
        print("   note: no CABLE capture tool (proto-audio.exe): cam_fallback (a) and cam_soak cannot hold a mic stream")
    if not info.get("ffmpeg"):
        print("   note: no ffmpeg on the PC: the 1080p last-rows grab of cam_latency is skipped")
    if problems:
        raise L.Stop("setup: " + "; ".join(problems))
    return info


def table():
    runs = A_RUNS()
    latest = {}
    for d in sorted(runs.glob("*/result.json")):
        r = json.loads(d.read_text())
        latest[r["case"]] = (d.parent.name, r)
    lip = lipsync_result()      # CW10's, one level deeper and in its own shape
    if lip:
        latest["cam_lipsync"] = (lip["dir"], lip)
    lines = ["# Camera E2E results (newest run of each case)", "",
             "Generated by `dev/camera/e2e/run.py table` from `runs/*/result.json` (raw data git-ignored).", "",
             "| case | verdict | commit | failing or unmeasured checks | run |", "|---|---|---|---|---|"]
    for name in list(C.ALL_ORDER) + ["cam_soak"] + list(C.REAL):
        if name not in latest:
            continue
        d, r = latest[name]
        bad = "; ".join(f"{c['name']} = {json.dumps(c['value'], default=str)[:60]}" for c in r["checks"] if c["pass"] is not True)
        lines.append(f"| {name} | {r['verdict']} | {r.get('commit') or ''} | {bad or '-'} | `{d}` |")
    text = "\n".join(lines) + "\n"
    (runs / "SUMMARY.md").write_text(text)
    print(text)


def A_RUNS():
    p = HERE / "runs"
    p.mkdir(exist_ok=True)
    return p


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command")
    ap.add_argument("rest", nargs="*")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--real", action="store_true")
    ap.add_argument("--with-soak", action="store_true")
    ap.add_argument("--no-preflight", action="store_true")
    ap.add_argument("--keep-going", action="store_true")
    ap.add_argument("--yes", action="store_true")
    ap.add_argument("--cycles", type=int)
    ap.add_argument("--seconds", type=float)
    ap.add_argument("--qualities")
    ap.add_argument("--parts")
    ap.add_argument("--reps", type=int)
    ap.add_argument("--drops", type=int)
    a = ap.parse_args()
    cmd = a.command
    o = opts_of(a)
    known = set(C.CASES) | {"cam_oldpeer", "cam_lipsync", "cam_lipsync_real", "list", "preflight", "setup", "all", "soak",
                            "smoke", "real", "analyze", "table", "idle", "replay-c4", "selftest"}
    if cmd not in known:
        ap.error(f"unknown command {cmd!r}; `run.py list` shows the cases")

    if cmd == "list":
        print(f"{'case':18} {'min':>5}  needs")
        for name in C.ALL_ORDER + ("cam_soak",) + C.REAL:
            if name in C.CASES:
                print(f"{name:18} {C.CASES[name][2]:5.1f}  {C.CASES[name][3]}")
            elif name == "cam_oldpeer":
                print(f"{name:18} {0.5:5.1f}  PC log only (asusrog online)")
            else:
                print(f"{name:18} {'?':>5}  CW10's script {'(real camera, H1)' if 'real' in name else '(present)' if (HERE / 'cam_lipsync.py').exists() else '(not there yet)'}")
        return 0
    if cmd == "selftest":
        return subprocess.run([sys.executable, "-m", "unittest", "-v", "test_analysis"], cwd=HERE).returncode
    if cmd == "replay-c4":
        import replay_c4
        return replay_c4.main(a.rest)
    if cmd == "analyze":
        for d in a.rest:
            analyze_dir(Path(d) if Path(d).exists() else HERE / "runs" / d)
        return 0
    if cmd == "table":
        table()
        return 0

    if cmd == "smoke":
        seq = [("cam_start", {"cycles": 3}), ("cam_latency", {"seconds": 60, "qualities": ("720p",), "no_lastrows": True})]
    elif cmd == "all":
        seq = [(n, o) for n in C.ALL_ORDER] + ([("cam_soak", o)] if a.with_soak else [])
    elif cmd == "soak":
        seq = [("cam_soak", o)]
    elif cmd == "real":
        seq = [("cam_start_real", o), ("cam_latency_real", o), ("cam_lipsync_real", o)]
    elif cmd in ("preflight", "setup", "idle"):
        seq = []
    else:
        seq = [(cmd, o)]
    real = cmd == "real" or cmd in C.REAL or a.real

    if a.dry_run:
        total = 0.0
        for name, oo in seq:
            if name == "cam_oldpeer":
                oldpeer(None, dry=True)
                total += 0.5
            elif name in ("cam_lipsync", "cam_lipsync_real"):
                lipsync(a.rest, dry=True, real=name.endswith("real"))
            else:
                run_case(name, oo, None, dry=True)
                total += minutes_of(C.CASES[name][0](C.Ids(), oo))
            print()
        if seq:
            print(f"total ~{total:.0f} min (+ pre-flight/setup ~1 min){'; cam_lipsync not counted' if any(n == 'cam_lipsync' for n, _ in seq) else ''}")
        else:
            print(f"{cmd}: nothing to plan in a dry run")
        return 0

    import camlib as L
    import preflight
    try:
        if cmd == "idle":
            print("idle:", L.teardown() or "done")
            return 0
        L.lock()
        try:
            ctx = None
            if not a.no_preflight or cmd == "preflight":
                rep, ctx = preflight.run(real=real)
                if rep.blocked():
                    print("\nSTOP: pre-flight failed:\n  - " + "\n  - ".join(f"{r['check']}: {r['detail']}" for r in rep.blocked()))
                    return 2
            if cmd == "preflight":
                return 0
            setup()
            if cmd == "setup":
                return 0
            ctx = ctx or L.Ctx()
            results = []
            for name, oo in seq:
                try:
                    if name == "cam_oldpeer":
                        results.append(oldpeer(ctx))
                    elif name in ("cam_lipsync", "cam_lipsync_real"):
                        results.append(lipsync(a.rest, real=name.endswith("real")))
                    else:
                        results.append(run_case(name, oo, ctx, assume_yes=a.yes))
                except L.Stop:
                    raise
                except Exception as error:  # noqa: BLE001
                    print(f"   {name}: harness error {error!r}")
                    results.append({"case": name, "verdict": "error", "checks": []})
                    if not a.keep_going and cmd not in ("all", "real"):
                        raise
                time.sleep(3)
            if len(seq) > 1:
                table()
            return 0 if all(r and r.get("verdict") in ("pass", "skipped") for r in results) else 1
        finally:
            L.unlock()
    except L.Stop as error:
        print(f"\nSTOP: {error}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
