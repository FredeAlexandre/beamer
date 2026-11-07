"""Stdio helpers that put a timeline on the audio flowing through a scenario (Mac and PC).

  tap.py rec --out P [--channels N] [--seconds S] -- CAPTURE_CMD...
      Run a capture command and record what it delivers (sink-side detector).
  tap.py src --out P [--channels N] [--mix A|B ...] [exclusion] -- CAPTURE_CMD...
      Run a capture command, optionally add a test signal to it, and forward it on stdout (a
      `peer.py --source cmd:...`). Records what it forwards.
  tap.py sink --out P [--channels N] [--gain-db G] [-- PLAY_CMD...]
      Record what arrives on stdin (a `peer.py --sink cmd:...`: the jitter buffer's paced output)
      and optionally forward it, scaled, to a player.
  tap.py gen --out P --sig A|B [--channels N] [--seconds S] [--delay S] ...
      Write a test signal paced in real time on stdout (pipe it into a `play`, i.e. an app that
      plays sound). Logs each marker's time.

Recorded per run prefix P: P.f32 (mono mix of the stream, f32le 48 kHz), P.chunks (float64 pairs:
stream index after the chunk, time.monotonic() when it was read), P.log (JSON lines: start,
markers, exit), P.child.err (the capture tool's own JSON diagnostics). time.monotonic() is
mach_absolute_time on the Mac and QueryPerformanceCounter on Windows (Python 3.13), the clocks
the transport's ping exchange relates.

Exclusion helpers for capture-system (scenario 5):
  --exclude-sibling PATTERN   Mac: wait for a sibling process (same parent, `pgrep -f PATTERN`),
                              e.g. the transport's `beamer-audio play`, and pass --exclude-pid.
  --exclude-root-python       Windows: pass --exclude-pid of the topmost python.exe ancestor that is
                              a real interpreter, not a venv launcher (the transport process,
                              which spawned the player).
"""
from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sig import RATE, Injector  # noqa: E402

mono = time.monotonic


class Log:
    def __init__(self, path):
        self.f = open(path, "a", buffering=1)

    def __call__(self, **kw):
        kw.setdefault("t", mono())
        self.f.write(json.dumps(kw) + "\n")


def find_sibling(pattern, timeout=5.0):
    ppid = os.getppid()
    end = mono() + timeout
    while mono() < end:
        r = subprocess.run(["pgrep", "-P", str(ppid), "-f", pattern], capture_output=True, text=True)
        pids = [int(p) for p in r.stdout.split() if int(p) != os.getpid()]
        if pids:
            return pids[0]
        time.sleep(0.05)
    return None


def win_root_python():
    import ctypes
    from ctypes import wintypes

    class PE(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                    ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wintypes.DWORD),
                    ("cntThreads", wintypes.DWORD), ("th32ParentProcessID", wintypes.DWORD),
                    ("pcPriClassBase", ctypes.c_long), ("dwFlags", wintypes.DWORD), ("szExeFile", ctypes.c_wchar * 260)]
    k = ctypes.windll.kernel32
    k.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    h = k.CreateToolhelp32Snapshot(2, 0)
    e = PE()
    e.dwSize = ctypes.sizeof(PE)
    m = {}
    ok = k.Process32FirstW(h, ctypes.byref(e))
    while ok:
        m[e.th32ProcessID] = (e.th32ParentProcessID, e.szExeFile.lower())
        ok = k.Process32NextW(h, ctypes.byref(e))
    k.CloseHandle(h)

    def image(pid):
        hp = k.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not hp:
            return ""
        buf = ctypes.create_unicode_buffer(1024)
        n = wintypes.DWORD(1024)
        ok = k.QueryFullProcessImageNameW(hp, 0, buf, ctypes.byref(n))
        k.CloseHandle(hp)
        return buf.value if ok else ""
    # Walk up through python.exe ancestors: tap.py <- (venv launcher) <- peer.py <- (venv launcher).
    # Process loopback does NOT treat a venv launcher's child as part of the launcher's tree
    # (tested: excluding the launcher left the grandchild player audible; excluding the base
    # interpreter that spawned the player silenced it), so pick the topmost base interpreter.
    cur, chain, best = os.getpid(), [], None
    while cur in m and m[cur][1] == "python.exe":
        img = image(cur)
        chain.append((cur, img))
        if not img.lower().endswith("\\scripts\\python.exe"):
            best = cur
        cur = m[cur][0]
    return best, chain


def run_capture(a, log):
    cmd = list(a.cmd)
    if a.exclude_sibling:
        pid = find_sibling(a.exclude_sibling)
        log(event="exclude", sibling=pid, pattern=a.exclude_sibling)
        if pid:
            cmd += ["--exclude-pid", str(pid)]
    if a.exclude_root_python:
        pid, chain = win_root_python()
        log(event="exclude", root=pid, chain=chain)
        cmd += ["--exclude-pid", str(pid)]
    log(event="start", mode=a.mode, cmd=cmd, pid=os.getpid())
    err = open(a.out + ".child.err", "ab")
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=err, bufsize=0)
    if sys.platform != "win32":
        def term(*_):
            p.terminate()  # the capture tool cleans up (tap, mute) and logs its CPU
        signal.signal(signal.SIGTERM, term)
        signal.signal(signal.SIGINT, term)
    inj = None
    if a.mix:
        inj = Injector(a.mix, a.pilot_amp, a.marker_amp, a.period, a.start, a.count)
    fd = p.stdout.fileno()
    fsz = 4 * a.channels
    fs = open(a.out + ".f32", "wb", buffering=0)
    fc = open(a.out + ".chunks", "wb", buffering=0)
    out = sys.stdout.buffer if a.mode == "src" else None
    rest, idx = b"", 0
    end = mono() + a.seconds if a.seconds else None
    while end is None or mono() < end:
        try:
            b = os.read(fd, 65536)
        except OSError:
            break
        t = mono()
        if not b:
            break
        b = rest + b
        n = len(b) // fsz * fsz
        rest = b[n:]
        if not n:
            continue
        x = np.frombuffer(b[:n], "<f4").reshape(-1, a.channels)
        if inj is not None:
            y, starts = inj.render(idx, len(x))
            x = x + y[:, None]
            for m in starts:
                log(event="marker", index=m)
        if out is not None:
            try:
                out.write(x.astype("<f4").tobytes())
                out.flush()
            except (BrokenPipeError, OSError):
                break
        fs.write(x.mean(axis=1).astype("<f4").tobytes())
        idx += len(x)
        fc.write(np.array([idx, t], "<f8").tobytes())
    p.stdout.close()
    try:
        p.wait(timeout=3)
    except subprocess.TimeoutExpired:
        p.terminate()
    log(event="exit", frames=idx, cpu_s=round(time.process_time(), 3), child_rc=p.returncode)


def run_sink(a, log):
    """Record what arrives on stdin (the transport's paced output: an ideal playback device) and,
    if a command is given, forward it scaled by --gain-db to that player's stdin."""
    log(event="start", mode="sink", cmd=a.cmd, gain_db=a.gain_db, pid=os.getpid())
    p = None
    if a.cmd:
        p = subprocess.Popen(a.cmd, stdin=subprocess.PIPE, stderr=open(a.out + ".child.err", "ab"), bufsize=0)
    g = 10 ** (a.gain_db / 20)
    fd = sys.stdin.buffer.fileno()
    fsz = 4 * a.channels
    fs = open(a.out + ".f32", "wb", buffering=0)
    fc = open(a.out + ".chunks", "wb", buffering=0)
    rest, idx = b"", 0
    while True:
        b = os.read(fd, 65536)
        t = mono()
        if not b:
            break
        b = rest + b
        n = len(b) // fsz * fsz
        rest = b[n:]
        if not n:
            continue
        x = np.frombuffer(b[:n], "<f4").reshape(-1, a.channels)
        if p is not None:
            try:
                p.stdin.write((x * g).astype("<f4").tobytes())
            except (BrokenPipeError, OSError):
                p = None
        fs.write(x.mean(axis=1).astype("<f4").tobytes())
        idx += len(x)
        fc.write(np.array([idx, t], "<f8").tobytes())
    if p is not None:
        p.stdin.close()
        p.wait(timeout=5)
    log(event="exit", frames=idx, cpu_s=round(time.process_time(), 3))


def run_gen(a, log):
    if a.delay:
        time.sleep(a.delay)
    inj = Injector(a.sig, a.pilot_amp, a.marker_amp, a.period, a.start, a.count)
    out = sys.stdout.buffer
    B = 480
    total = int(a.seconds * RATE)
    log(event="start", mode="gen", sig=a.sig, pid=os.getpid())
    t0 = mono() + 0.02
    s = 0
    while s < total:
        due = t0 + s / RATE
        d = due - mono()
        if d > 0:
            time.sleep(d)
        x, starts = inj.render(s, B)
        tw = mono()
        try:
            out.write(np.repeat(x[:, None], a.channels, axis=1).astype("<f4").tobytes())
            out.flush()
        except (BrokenPipeError, OSError):
            break
        for m in starts:
            log(event="marker", index=m, t=tw + (m - s) / RATE)
        s += B
    log(event="exit", frames=s, cpu_s=round(time.process_time(), 3))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["rec", "src", "gen", "sink"])
    ap.add_argument("--gain-db", type=float, default=0.0)
    ap.add_argument("--out", required=True)
    ap.add_argument("--channels", type=int, default=1)
    ap.add_argument("--seconds", type=float, default=0)
    ap.add_argument("--delay", type=float, default=0)
    ap.add_argument("--mix", choices=["A", "B"], default=None)
    ap.add_argument("--sig", choices=["A", "B"], default="A")
    ap.add_argument("--pilot-amp", type=float, default=0.02)
    ap.add_argument("--marker-amp", type=float, default=0.1)
    ap.add_argument("--period", type=float, default=1.0)
    ap.add_argument("--start", type=float, default=2.0)
    ap.add_argument("--count", type=int, default=None)
    ap.add_argument("--exclude-sibling", default=None)
    ap.add_argument("--exclude-root-python", action="store_true")
    argv = sys.argv[1:]
    cmd = argv[argv.index("--") + 1:] if "--" in argv else []
    a = ap.parse_args(argv[:argv.index("--")] if "--" in argv else argv)
    a.cmd = cmd
    log = Log(a.out + ".log")
    if a.mode == "gen":
        run_gen(a, log)
    elif a.mode == "sink":
        run_sink(a, log)
    else:
        run_capture(a, log)


if __name__ == "__main__":
    main()
