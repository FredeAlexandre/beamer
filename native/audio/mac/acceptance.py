# SPDX-License-Identifier: GPL-3.0-only
"""Live acceptance checks of the macOS backend (PHASE2 §13 WP3) on this Mac.

    uv run --python 3.13 --with numpy native/audio/mac/acceptance.py [test ...] [--json out.json]

Tests (default: all but `restart`):
  list     `backend_check list` and `beamer-audio list`: built-in mic/speakers with forms, never the
           virtual mic or its feed as a source/output
  mute     system tap with mute: tap level = source, the built-in mic hears only its noise floor
           (AUDIBLE: a 1 kHz tone from afplay at the current volume, 3 x 4 s)
  kill     the local mute lifts <= 1 s after the capturing process is SIGKILLed (AUDIBLE, 6 s)
  selftap  a tone rendered by the backend's own process is not in its system tap (<= -90 dBFS); the
           same tone from another process is (silent: rendered into the hidden virtual-mic feed)
  demand   virtual_mic.in_use true <= 200 ms after an app opens "Beamer Microphone", false 3 s after it
           closes, never set by the feed alone
  restart  feed IO resumes <= 3 s after `sudo -n beamer-hal restart` and an app hears it again
  cpu      `beamer-audio` sending the built-in mic to a null peer: < 1 % of a core (needs the engine)

Needs: native/audio/build-mac.sh output, the prototype tool dev/audio/proto/mac/.build/beamer-audio
(the "app" that records Beamer Microphone), the installed driver. Changes no default device or volume.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
BC = Path(os.environ.get("WP3_BUILD", ROOT / "native/audio/build/mac")) / "backend_check"
BUILT = Path(os.environ.get("WP3_BUILD", ROOT / "native/audio/build/mac"))
HELPER = BUILT / "beamer-audio"
PROTO = ROOT / "dev/audio/proto/mac/.build/beamer-audio"
HAL = "/usr/local/libexec/beamer-hal"
RATE = 48000
results: dict = {}


def check(name, ok, **info):
    results[name] = {"ok": bool(ok), **info}
    print(f"[{'PASS' if ok else 'FAIL'}] {name} {json.dumps(info)}", flush=True)


def lines_of(p: subprocess.Popen, sink: list, stamp=True):
    """Collect JSON lines from p.stdout with the wall time they arrived."""
    def run():
        for raw in p.stdout:
            try:
                ev = json.loads(raw)
            except ValueError:
                continue
            ev["_wall"] = time.time()
            sink.append(ev)
    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t


def bc(*args, env=None) -> subprocess.Popen:
    return subprocess.Popen([str(BC), *args], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                            env={**os.environ, **(env or {})})


def stop(p: subprocess.Popen):
    if p.poll() is None:
        p.send_signal(signal.SIGINT)
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()


def tone_db(x: np.ndarray, hz=1000.0) -> float:
    if len(x) == 0:
        return -200.0
    t = np.arange(len(x)) / RATE
    a = 2 * abs(np.mean(x * np.exp(-2j * np.pi * hz * t)))
    return round(20 * np.log10(max(a, 1e-10)), 1)


def load(path: Path, ch: int) -> np.ndarray:
    x = np.fromfile(path, dtype="<f4").astype(np.float64)
    return x[: len(x) // ch * ch].reshape(-1, ch).mean(axis=1)


def tone_wav(path: Path, seconds: float, amp=0.125):
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                    f"sine=frequency=1000:duration={seconds}:sample_rate=48000", "-af", f"volume={amp / 0.125}",
                    "-ac", "2", str(path)], check=True)


# ------------------------------------------------------------------------------------------------

def helper_devices_event() -> str:
    """The helper's §3.3 `devices` event (control mode), as text."""
    p = subprocess.Popen([str(HELPER), "--control", "--parent-pid", str(os.getpid()), "--state-dir",
                          tempfile.mkdtemp(), "--exclude-pid", str(os.getpid())], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    try:
        for line in p.stdout:
            if '"ev":"devices"' in line.replace(" ", ""):
                return line
    finally:
        p.stdin.close()
        p.wait(5)
    return ""


def t_list():
    if HELPER.exists():  # `beamer-audio list` (diagnostics): the virtual mic and feed only with their roles
        dump = json.loads(subprocess.run([str(HELPER), "list"], capture_output=True, text=True, timeout=20).stdout)
        bad = [d["key"] for d in dump.get("devices", []) if d["key"].startswith("com.beamer.audio.")
               and d.get("role") == "normal"]
        check("list_beamer-audio_roles", not bad, devices=[(d["name"], d.get("role"), d.get("form"))
                                                         for d in dump.get("devices", [])])
    for name, exe in (("backend_check", BC), ("beamer-audio", HELPER)):
        if not exe.exists():
            check(f"list_{name}", False, error="not built")
            continue
        if exe == HELPER:
            out = helper_devices_event()
        else:
            out = subprocess.run([str(exe), "list"], capture_output=True, text=True, timeout=20).stdout
        ev = next((json.loads(l) for l in out.splitlines() if l.startswith("{") and '"inputs"' in l), None)
        if ev is None:
            check(f"list_{name}", False, output=out[:400])
            continue
        ins = {d["key"]: d for d in ev["inputs"]}
        outs = {d["key"]: d for d in ev["outputs"]}
        vm = [k for k in list(ins) + list(outs) if k.startswith("com.beamer.audio.")]
        ok = (ins.get("BuiltInMicrophoneDevice", {}).get("form") == "microphone"
              and outs.get("BuiltInSpeakerDevice", {}).get("form") == "speakers" and not vm)
        check(f"list_{name}", ok, inputs=[(d["name"], d["form"]) for d in ev["inputs"]],
              outputs=[(d["name"], d["form"]) for d in ev["outputs"]], virtual_listed=vm,
              virtual_mic=ev.get("virtual_mic"))


def t_mute(tmp: Path):
    wav = tmp / "tone4.wav"
    tone_wav(wav, 4)
    lv = {}
    for mode in ("baseline", "unmuted", "muted"):
        mic = bc("capture", "--kind", "mic", "--seconds", "7", "--out", str(tmp / f"mic_{mode}.f32"))
        sysp = None
        if mode != "baseline":
            a = ["capture", "--kind", "system", "--seconds", "7", "--out", str(tmp / f"sys_{mode}.f32")]
            sysp = bc(*a, *(["--mute-local"] if mode == "muted" else []))
        time.sleep(1.5)
        if mode == "baseline":
            time.sleep(4)
        else:
            subprocess.run(["afplay", str(wav)], check=True)
        for p in (mic, sysp):
            if p:
                p.wait(15)
        w = slice(int(2.5 * RATE), int(5.0 * RATE))  # inside the tone (it plays from ~1.5 s to ~5.5 s)
        lv[mode] = {"mic": tone_db(load(tmp / f"mic_{mode}.f32", 1)[w])}
        if sysp:
            lv[mode]["tap"] = tone_db(load(tmp / f"sys_{mode}.f32", 2)[w])
    raw = subprocess.run(["ffmpeg", "-loglevel", "error", "-i", str(wav), "-f", "f32le", "-ac", "2", "-"],
                         capture_output=True, check=True).stdout
    src = tone_db(np.frombuffer(raw, dtype="<f4").astype(np.float64).reshape(-1, 2).mean(axis=1)[RATE:3 * RATE])
    ok = (abs(lv["muted"]["tap"] - lv["unmuted"]["tap"]) <= 0.5 and abs(lv["muted"]["tap"] - src) <= 1.0
          and lv["unmuted"]["mic"] - lv["muted"]["mic"] >= 30 and lv["muted"]["mic"] <= lv["baseline"]["mic"] + 6)
    check("mute_local", ok, source_file_tone_dbfs=src, tone_levels=lv)


def t_kill(tmp: Path):
    wav = tmp / "tone6.wav"
    tone_wav(wav, 6)
    ev: list = []
    mic = bc("capture", "--kind", "mic", "--seconds", "10", "--out", str(tmp / "mic_kill.f32"))
    lines_of(mic, ev)
    sysp = bc("capture", "--kind", "system", "--mute-local", "--seconds", "20")
    time.sleep(1.0)
    start = next(e["_wall"] for e in ev if e.get("ev") == "capturing")
    play = subprocess.Popen(["afplay", str(wav)])
    t_play = time.time()
    time.sleep(3.0)
    sysp.kill()
    t_kill = time.time()
    sysp.wait()
    play.wait()
    mic.wait(15)
    x = load(tmp / "mic_kill.f32", 1)
    hop = RATE // 20  # 50 ms windows
    lv = [tone_db(x[i:i + hop]) for i in range(0, len(x) - hop, hop)]
    k = int((t_kill - start) * 20)
    p0 = int((t_play - start) * 20)
    muted = float(np.median(lv[p0 + 4:k])) if k > p0 + 4 else None
    after = lv[k:]
    loud = next((i for i, v in enumerate(after) if muted is not None and v > muted + 20), None)
    unmute_ms = None if loud is None else loud * 50
    check("unmute_after_sigkill", unmute_ms is not None and unmute_ms <= 1000, unmute_ms=unmute_ms,
          muted_tone_dbfs=muted, after_kill_dbfs=after[: 30])


def t_selftap():
    out = subprocess.run([str(BC), "selftap", "--to", "vmic", "--tone", "1000", "--level", "-20", "--seconds", "3"],
                         capture_output=True, text=True, timeout=30).stdout
    own = next(json.loads(l) for l in out.splitlines() if '"selftap"' in l)
    ev: list = []
    player = bc("play", "--vmic", "--tone", "1000", "--level", "-20", "--seconds", "6")
    time.sleep(0.7)
    cap = subprocess.run([str(BC), "capture", "--kind", "system", "--tone", "1000", "--seconds", "3"],
                         capture_output=True, text=True, timeout=30).stdout
    other = next(json.loads(l) for l in cap.splitlines() if '"captured"' in l)
    stop(player)
    check("self_exclusion", own["tone_dbfs"] <= -90 and other["tone_dbfs"] > -23,
          own_tone_in_own_tap_dbfs=own["tone_dbfs"], own_tap_rms_dbfs=own["rms_dbfs"],
          other_process_tone_in_tap_dbfs=other["tone_dbfs"])


def recorder() -> subprocess.Popen:
    return subprocess.Popen([str(PROTO), "capture-mic", "--device", "com.beamer.audio.mic", "--channels", "1",
                             "--rate", "48000", "--stats", "0"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)


def t_demand():
    ev: list = []
    w = bc("watch", "--seconds", "16")
    lines_of(w, ev)
    time.sleep(1.0)
    feed = bc("play", "--vmic", "--tone", "440", "--level", "-30", "--seconds", "14")  # Beamer's feed alone
    time.sleep(2.5)
    feed_only = [e for e in ev if e.get("ev") == "vmic_in_use"]
    rec = recorder()
    t_open = time.time()
    first = rec.stdout.read(4)  # first audio from "Beamer Microphone" = the device is running
    t_audio = time.time()
    time.sleep(3.0)
    rec.send_signal(signal.SIGINT)
    rec.wait()
    t_close = time.time()
    time.sleep(4.5)
    stop(feed)
    stop(w)
    on = next((e for e in ev if e.get("ev") == "vmic_in_use" and e["in_use"] and e["_wall"] > t_open), None)
    off = next((e for e in ev if e.get("ev") == "vmic_in_use" and not e["in_use"] and e["_wall"] > t_close), None)
    on_ms = None if on is None else round((on["wall"] - t_open) * 1000)
    on_after_audio_ms = None if on is None else round((on["wall"] - t_audio) * 1000)
    off_ms = None if off is None else round((off["wall"] - t_close) * 1000)
    check("vmic_demand", not feed_only and on_ms is not None and on_ms <= 200 and off_ms is not None
          and 2900 <= off_ms <= 3600 and len(first) == 4,
          feed_alone_events=len(feed_only), on_ms_after_recorder_spawn=on_ms, on_ms_after_first_audio=on_after_audio_ms,
          off_ms_after_recorder_exit=off_ms)


def t_restart():
    ev: list = []
    feed = bc("play", "--vmic", "--report-io", "--tone", "1025", "--level", "-20", "--seconds", "30")
    lines_of(feed, ev)
    time.sleep(2.0)
    t_r = time.time()
    r = subprocess.run(["sudo", "-n", HAL, "restart"], capture_output=True, text=True, timeout=60)
    deadline = time.time() + 10
    resumed = None
    while time.time() < deadline and resumed is None:
        resumed = next((e for e in ev if e.get("ev") == "io_resumed"), None)
        time.sleep(0.05)
    resume_s = None if resumed is None else round(resumed["_wall"] - t_r, 2)
    time.sleep(1.0)
    rec = recorder()
    buf = b""
    while len(buf) < 4 * RATE * 2:
        b = rec.stdout.read(65536)
        if not b:
            break
        buf += b
    stop(rec)
    stop(feed)
    x = np.frombuffer(buf[: len(buf) // 4 * 4], dtype="<f4").astype(np.float64)[RATE // 2:]
    lvl = tone_db(x, 1025)
    check("feed_recovers_after_coreaudiod_restart", r.returncode == 0 and resume_s is not None and resume_s <= 3.0
          and abs(lvl + 20) < 1.0, helper_rc=r.returncode, io_resumed_s_after_restart_cmd=resume_s,
          app_hears_tone_dbfs=lvl, events=[{k: v for k, v in e.items() if k != "_wall"} for e in ev
                                           if e.get("ev") in ("io_stopped", "io_resumed", "stream_lost")])


class Helper:
    """beamer-audio --control driven over stdio."""

    def __init__(self, *extra):
        self.p = subprocess.Popen([str(HELPER), "--control", "--parent-pid", str(os.getpid()), "--state-dir",
                                   tempfile.mkdtemp(), "--exclude-pid", str(os.getpid()), *extra],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self.ev: list = []
        lines_of(self.p, self.ev)
        self.req = 0

    def send(self, cmd: dict, wait=True):
        self.req += 1
        cmd = {**cmd, "req": self.req}
        self.p.stdin.write(json.dumps(cmd) + "\n")
        self.p.stdin.flush()
        if not wait:
            return None
        deadline = time.time() + 10
        while time.time() < deadline:
            for e in list(self.ev):
                if e.get("req") == self.req and e.get("ev") in ("ok", "error"):
                    return e
            time.sleep(0.02)
        return None

    def wait_ev(self, name, timeout=5):
        deadline = time.time() + timeout
        while time.time() < deadline:
            for e in list(self.ev):
                if e.get("ev") == name:
                    return e
            time.sleep(0.02)
        return None

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(5)
        except Exception:
            self.p.kill()


def cpu_of(pid: int) -> float:
    out = subprocess.run(["ps", "-o", "time=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    sec = 0.0
    for part in out.split(":"):
        sec = sec * 60 + float(part)
    return sec


def t_cpu():
    if not HELPER.exists():
        check("mic_send_cpu", False, error="beamer-audio not built (engine missing)")
        return
    a, b = Helper(), Helper("--backend", "null:out=null;vmic=null")
    try:
        if not (a.wait_ev("ready") and b.wait_ev("ready")):
            check("mic_send_cpu", False, error="no ready")
            return
        key = base64.b64encode(os.urandom(32)).decode()
        skey = base64.b64encode(os.urandom(32)).decode()
        r1 = b.send({"cmd": "flow_open", "flow": "0000000b", "peer": "QQ==", "role": "answer", "port": 0,
                     "remote": None, "key": key, "rr_ms": 10})
        pb = b.wait_ev("flow_bound")["port"]
        r2 = a.send({"cmd": "flow_open", "flow": "0000000b", "peer": "Qg==", "role": "offer", "port": 0,
                     "remote": {"host": "127.0.0.1", "port": pb}, "key": key, "rr_ms": 10})
        pa = next(e for e in a.ev if e.get("ev") == "flow_bound")["port"]
        b.send({"cmd": "flow_remote", "flow": "0000000b", "host": "127.0.0.1", "port": pa})
        codec = {"name": "opus", "app": "voip", "channels": 1, "bitrate": 32000, "frame_ms": 10, "fec": True}
        r3 = b.send({"cmd": "recv", "stream": 7, "flow": "0000000b", "kind": "mic", "codec": codec, "key": skey,
                     "sink": {"virtual_mic": True}})
        r4 = a.send({"cmd": "send", "stream": 7, "flow": "0000000b", "kind": "mic",
                     "source": {"device": "BuiltInMicrophoneDevice"}, "codec": codec, "key": skey,
                     "mute_local": False, "aec": None})
        time.sleep(3)
        c0, t0 = cpu_of(a.p.pid), time.time()
        time.sleep(30)
        pct = 100 * (cpu_of(a.p.pid) - c0) / (time.time() - t0)
        states = [e for e in a.ev if e.get("ev") in ("stream_state", "flow_state")]
        stats = [e for e in a.ev if e.get("ev") == "stats"][-1:] + [e for e in b.ev if e.get("ev") == "stats"][-1:]
        running = any(e.get("ev") == "stream_state" and e.get("state") == "running" for e in states)
        check("mic_send_cpu", running and pct < 1.0, cpu_pct=round(pct, 2), replies=[r1, r2, r3, r4],
              states=[{k: v for k, v in e.items() if k != "_wall"} for e in states], last_stats=stats)
    finally:
        a.close()
        b.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tests", nargs="*")
    ap.add_argument("--json")
    args = ap.parse_args()
    tests = args.tests or ["list", "selftap", "demand", "mute", "kill", "cpu"]
    for p in (BC, PROTO):
        if not p.exists():
            sys.exit(f"missing {p}")
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        for t in tests:
            print(f"--- {t}", flush=True)
            fn = globals()[f"t_{t}"]
            fn(tmp) if t in ("mute", "kill") else fn()
    failed = [k for k, v in results.items() if not v["ok"]]
    print(f"\n{len(results) - len(failed)} passed, {len(failed)} failed" + (f": {failed}" if failed else ""))
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=2))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
