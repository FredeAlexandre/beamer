# SPDX-License-Identifier: GPL-3.0-only
"""Live checks of the installed BeamerAudio.driver (README "What remains to verify", 3-7 and 9).

Drives the real devices through the prototype tool `beamer-audio`: a feeder plays a 1025 Hz pilot
with chirp markers into the hidden "Beamer Mic Feed", a recorder captures "Beamer Microphone".

    uv run --python 3.13 --with numpy test/live_check.py [--skip-restart] [--json out.json]

Needs the driver installed (sudo -n /usr/local/libexec/beamer-hal install) and, for the coreaudiod
restart step, the beamer-hal sudo rule. Writes nothing outside build/.
"""
from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
DRIVER = HERE.parent
sys.path.insert(0, str(DRIVER.parent / "scenarios"))
import sig  # noqa: E402  (shared test signals: pilot + chirp markers, lock-in glitch detector)

TOOL = DRIVER.parent / "mac" / ".build" / "beamer-audio"
CA_UTIL = DRIVER / "build" / "ca_util"
HELPER = "/usr/local/libexec/beamer-hal"
MIC, FEED = "com.beamer.audio.mic", "com.beamer.audio.micfeed"
RATE = 48000
PILOT = sig.SIGS["A"]["pilot"]

results: dict = {}
failures: list[str] = []


def step(msg):
    print(f"--- {msg}", flush=True)


def check(name, ok, **info):
    results[name] = {"ok": bool(ok), **info}
    print(f"[{'PASS' if ok else 'FAIL'}] {name} {json.dumps(info)}", flush=True)
    if not ok:
        failures.append(name)


def ca(*a) -> str:
    # timeout: Core Audio calls block while coreaudiod is restarting or stuck
    return subprocess.run([str(CA_UTIL), *a], check=True, capture_output=True, text=True, timeout=20).stdout.strip()


class Feeder:
    """beamer-audio play into the feed; a thread writes the pilot as fast as the device takes it."""

    def __init__(self, buffer_frames=512, rate=RATE):
        self.rate = rate
        self.inj = sig.Injector("A", pilot_amp=0.1, marker_amp=0.3, period=1.0, start=0.5)
        self.p = subprocess.Popen([str(TOOL), "play", "--device", FEED, "--channels", "1", "--rate", str(rate),
                                   "--stats", "0", "--buffer-frames", str(buffer_frames), "--prefill-ms", "10"],
                                  stdin=subprocess.PIPE, stderr=subprocess.PIPE)
        self.markers: list[int] = []
        self.stop_ev = threading.Event()
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _run(self):
        s0, n = 0, 480
        try:
            while not self.stop_ev.is_set():
                x, starts = self.inj.render(s0, n)
                self.markers += starts
                self.p.stdin.write(x.tobytes())
                s0 += n
        except (BrokenPipeError, ValueError, OSError):
            pass

    def kill(self):
        """SIGKILL: Beamer crashing or being force-quit, no clean stop of the IOProc."""
        self.stop_ev.set()
        self.p.kill()
        self.p.wait()
        self.t.join(5)
        self._close()

    def _close(self):
        try:
            self.p.stdin.close()
        except OSError:   # the player is gone: flushing the pipe raises BrokenPipeError
            pass

    def stop(self):
        # Signal first: the writer thread may be blocked in write() holding the pipe's lock, so
        # closing stdin from here would wait for it forever. The exit makes its write fail.
        self.stop_ev.set()
        self.p.send_signal(signal.SIGINT)
        try:
            self.p.wait(5)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
        self.t.join(5)
        self._close()


class Recorder:
    """beamer-audio capture-mic on Beamer Microphone; keeps the samples and when they arrived."""

    def __init__(self, buffer_frames=512):
        self.p = subprocess.Popen([str(TOOL), "capture-mic", "--device", MIC, "--channels", "1", "--rate", str(RATE),
                                   "--stats", "0", "--buffer-frames", str(buffer_frames)],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.buf = bytearray()
        self.lock = threading.Lock()
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _run(self):
        while True:
            b = self.p.stdout.read1(65536) if hasattr(self.p.stdout, "read1") else self.p.stdout.read(4096)
            if not b:
                return
            with self.lock:
                self.buf += b

    def mark(self) -> int:
        """Sample index captured so far (a timeline marker for phases)."""
        with self.lock:
            return len(self.buf) // 4

    def samples(self) -> np.ndarray:
        with self.lock:
            n = len(self.buf) // 4 * 4
            return np.frombuffer(bytes(self.buf[:n]), dtype="<f4").astype(np.float64)

    def alive(self) -> bool:
        return self.p.poll() is None

    def stop(self) -> str:
        if self.p.poll() is None:
            self.p.send_signal(signal.SIGINT)
            try:
                self.p.wait(5)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
        self.t.join(2)
        return self.p.stderr.read().decode(errors="replace")


def wait_samples(rec: Recorder, seconds: float):
    """Wait until the recorder has captured `seconds` more audio (or 3x the time passed)."""
    target = rec.mark() + int(seconds * RATE)
    deadline = time.monotonic() + 3 * seconds + 1
    while rec.mark() < target and time.monotonic() < deadline:
        time.sleep(0.05)


def pilot_stats(x: np.ndarray) -> dict:
    g = sig.pilot_glitches(x, PILOT, skip_s=0.5)
    return {k: g.get(k) for k in ("seconds", "glitch_events", "pilot_dbfs", "dphi_p99", "glitch_at_s")}


def marker_offsets(x: np.ndarray, base: int, feeder: Feeder) -> list[int]:
    """Recorded index minus generated index for every marker found: constant within one feeder run
    means no sample was lost or repeated between the feed and the microphone."""
    # x starts at recorder index `base`, taken just before the feeder started, so a marker
    # generated at feeder index g shows up at x index g + latency (latency << the 1 s spacing).
    offs = []
    for j, _, _ in sig.detect(x, "A", thr=0.9):
        lag = [j - g for g in feeder.markers if 0 <= j - g < RATE // 2]
        if lag:
            offs.append(base + lag[0])   # recorder index minus feeder index
    return offs


def proc_cpu_seconds(pid: int) -> float:
    out = subprocess.run(["ps", "-o", "time=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    if not out:
        return float("nan")
    days = 0
    if "-" in out:
        d, out = out.split("-", 1)
        days = int(d)
    parts = [float(p) for p in out.split(":")]
    sec = 0.0
    for p in parts:
        sec = sec * 60 + p
    return days * 86400 + sec


def audio_pids() -> dict:
    def pg(*a):
        r = subprocess.run(["pgrep", *a], capture_output=True, text=True).stdout.split()
        return int(r[0]) if r else None
    return {"coreaudiod": pg("-x", "coreaudiod"), "driver_service": pg("-f", "Core Audio Driver \\(BeamerAudio.driver\\)")}


def cpu_over(seconds: float) -> dict:
    pids = audio_pids()
    a = {k: proc_cpu_seconds(v) for k, v in pids.items() if v}
    t0 = time.monotonic()
    time.sleep(seconds)
    dt = time.monotonic() - t0
    return {k: round(100 * (proc_cpu_seconds(pids[k]) - a[k]) / dt, 2) for k in a}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-restart", action="store_true", help="do not restart coreaudiod")
    ap.add_argument("--json", help="write results here")
    args = ap.parse_args()

    for p in (TOOL, CA_UTIL):
        if not p.exists():
            sys.exit(f"missing {p} (build it first)")

    # --- 2/3: visibility and UID translation from a user process
    check("feed_hidden_from_device_list", ca("visible", FEED) == "0" and ca("visible", MIC) == "1",
          mic_id=ca("id", MIC), feed_id=ca("id", FEED))
    if ca("rate", MIC) != "48000.0":
        ca("set-rate", MIC, "48000")
        time.sleep(1)

    cpu_idle = cpu_over(5)

    # --- 5a: microphone alone (feed never started) is digital silence
    step("mic alone")
    rec = Recorder()
    wait_samples(rec, 2.0)
    x = rec.samples()
    check("mic_alone_is_silent", len(x) > RATE and np.max(np.abs(x[RATE // 2:])) == 0.0,
          seconds=round(len(x) / RATE, 2), peak=float(np.max(np.abs(x))) if len(x) else None)

    # --- 4: feed started while the app records; continuity and sample accuracy
    step("feed started")
    a0 = rec.mark()
    feed = Feeder()
    wait_samples(rec, 12.0)
    a1 = rec.mark()
    x = rec.samples()[a0:a1]
    st = pilot_stats(x)
    offs = marker_offsets(x, a0, feed)
    check("feed_reaches_mic_without_glitch", st["glitch_events"] == 0 and st["pilot_dbfs"] is not None
          and abs(st["pilot_dbfs"] + 20.0) < 0.5, **st)
    check("no_lost_or_repeated_samples", len(offs) >= 8 and len(set(offs)) == 1, markers=len(offs),
          distinct_offsets=sorted(set(offs))[:5])
    cpu_running = cpu_over(5)

    # bit-exactness: the recording must equal the generated signal shifted by the marker offset
    if offs:
        o = offs[0]
        idx = np.arange(a0 + RATE * 2, a1 - RATE)  # well inside the run
        gen = np.concatenate([feed.inj.render(int(i - o), 4800)[0] for i in range(int(idx[0]), int(idx[-1]) - 4800, 4800)])
        got = rec.samples()[int(idx[0]):int(idx[0]) + len(gen)]
        err = float(np.max(np.abs(got - gen))) if len(got) == len(gen) else None
        check("bit_exact_loopback", err is not None and err == 0.0, max_abs_error=err, frames=len(gen))

    # --- 5b: Beamer killed while the app records: silence after the queued frames, no replay
    step("kill feeder")
    k0 = rec.mark()
    feed.kill()
    wait_samples(rec, 3.0)
    x = rec.samples()[k0:]
    nz = np.nonzero(x)[0]
    tail_ms = (nz[-1] + 1) / RATE * 1000 if len(nz) else 0.0
    check("feed_killed_mic_goes_silent", tail_ms < 400 and len(x) > 2 * RATE, audio_after_kill_ms=round(tail_ms, 1),
          silent_seconds=round((len(x) - (nz[-1] + 1 if len(nz) else 0)) / RATE, 2))

    # --- 5c: feed restarted; then the app leaves and a new one joins mid-stream
    step("restart feeder, app joins mid-stream")
    r0 = rec.mark()
    feed = Feeder()
    wait_samples(rec, 4.0)
    st = pilot_stats(rec.samples()[r0:])
    check("feed_restart_resumes", st["glitch_events"] == 0 and st["pilot_dbfs"] is not None, **st)
    rec.stop()
    time.sleep(0.5)
    rec = Recorder()
    wait_samples(rec, 4.0)
    st = pilot_stats(rec.samples())
    check("app_joins_mid_stream", st["glitch_events"] == 0 and st["pilot_dbfs"] is not None, **st)

    # --- 8: input volume and mute, set the way apps and System Settings do (device-level properties)
    step("volume and mute")
    vol = ca("volume", MIC).split()
    mute = ca("mute", MIC).split()
    levels = {}
    for name, cmd in (("half", ("volume", MIC, "0.5")), ("muted", ("mute", MIC, "1")),
                      ("unmuted", ("mute", MIC, "0")), ("full", ("volume", MIC, "1"))):
        ca(*cmd)
        m = rec.mark()
        wait_samples(rec, 1.5)
        x = rec.samples()[m + RATE // 2:]
        levels[name] = {"peak": float(np.max(np.abs(x))) if len(x) else None,
                        "pilot_dbfs": round(20 * float(np.log10(np.abs(sig.lockin(x, PILOT)).mean() + 1e-12)), 1)}
    check("volume_and_mute_controls", vol == ["1.0", "settable=1"] and mute == ["0", "settable=1"]
          and abs(levels["half"]["pilot_dbfs"] + 52.0) < 0.5 and levels["muted"]["peak"] == 0.0
          and abs(levels["unmuted"]["pilot_dbfs"] + 52.0) < 0.5 and abs(levels["full"]["pilot_dbfs"] + 20.0) < 0.5,
          initial={"volume": vol, "mute": mute}, levels=levels)

    # --- 7: sample-rate change on the microphone while both run: both devices follow
    step("rate change")
    ca("set-rate", MIC, "44100")
    time.sleep(1.5)
    rates = {"mic": ca("rate", MIC), "feed": ca("rate", FEED)}
    feed.stop(); rec.stop()
    rec, feed = Recorder(), Feeder()   # clients at 48 kHz: the HAL resamples to the device's 44.1 kHz
    wait_samples(rec, 5.0)
    st = pilot_stats(rec.samples())
    feed.stop(); rec.stop()
    ca("set-rate", MIC, "48000")
    time.sleep(1.5)
    back = {"mic": ca("rate", MIC), "feed": ca("rate", FEED)}
    check("rate_change_moves_both_devices", rates == {"mic": "44100.0", "feed": "44100.0"}
          and back == {"mic": "48000.0", "feed": "48000.0"} and st["glitch_events"] == 0 and st["pilot_dbfs"] is not None,
          during=rates, after=back, at_44k=st)

    # --- 6: coreaudiod restart while Beamer feeds and an app records
    step("coreaudiod restart")
    if not args.skip_restart:
        # a) restart while Beamer feeds and an app records (what beamer-hal install does to a busy Mac)
        rec, feed = Recorder(), Feeder()
        wait_samples(rec, 2.0)
        before = audio_pids()
        m = rec.mark()
        r = subprocess.run(["sudo", "-n", HELPER, "restart"], capture_output=True, text=True)
        time.sleep(3)
        got = rec.samples()[m:]
        spin = cpu_over(5)
        info = {"helper_rc": r.returncode, "recorder_alive": rec.alive(), "feeder_alive": feed.p.poll() is None,
                "frames_received_after_restart": len(got), "coreaudiod_cpu_pct_after": spin.get("coreaudiod")}
        feed.stop(); rec.stop()
        results["restart_with_clients"] = info
        print(f"[INFO] restart_with_clients {json.dumps(info)}", flush=True)
        # b) macOS 27's coreaudiod can stay at ~100 % CPU after being killed under load (same with
        # the built-in devices); a restart with no client running clears it.
        r = subprocess.run(["sudo", "-n", HELPER, "restart"], capture_output=True, text=True)
        time.sleep(5)
        after = audio_pids()
        ids = {"mic": ca("id", MIC), "feed": ca("id", FEED)}
        rec, feed = Recorder(), Feeder()
        wait_samples(rec, 4.0)
        st = pilot_stats(rec.samples())
        feed.stop(); rec.stop()
        check("coreaudiod_restart_devices_come_back", r.returncode == 0 and before["coreaudiod"] != after["coreaudiod"]
              and "0" not in ids.values() and st["glitch_events"] == 0 and st["pilot_dbfs"] is not None,
              pids_before=before, pids_after=after, ids_after=ids, fresh_pair=st)

    # --- 9: CPU of coreaudiod and of the driver's service process
    step("cpu")
    results["cpu_pct"] = {"idle": cpu_idle, "running": cpu_running}
    print(f"[INFO] cpu_pct {json.dumps(results['cpu_pct'])}")

    passed = sum(1 for v in results.values() if v.get("ok") is True)
    print(f"\n{passed} passed, {len(failures)} failed" + (f": {failures}" if failures else ""))
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=2))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
