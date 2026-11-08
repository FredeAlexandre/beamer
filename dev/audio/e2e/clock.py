# SPDX-License-Identifier: GPL-3.0-only
"""Mac <-> PC clock link over one persistent ssh session (no socket, no listener on either side).

  PC:  python clock.py serve        reads a line, answers with its time.monotonic() (QueryPerformanceCounter)
  Mac: ClockLink().start() ... .stop() -> pings [(t_mac, rtt_s, off_s)], off = PC clock - Mac clock

The pings feed `proto/scenarios/sig.clock_fit` exactly like Phase 1's transport pings: the lowest-RTT
30 % are fitted with off = a + b t; the absolute error is at most RTT_min / 2 (path asymmetry).
"""
from __future__ import annotations

import subprocess
import sys
import threading
import time


def serve():
    out = sys.stdout
    for line in sys.stdin:
        out.write(f"{line.strip()} {time.monotonic():.9f}\n")
        out.flush()


class ClockLink:
    def __init__(self, ssh_cmd, period=0.25):
        self.cmd = ssh_cmd
        self.period = period
        self.pings = []
        self._stop = threading.Event()
        self._thread = None
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen(self.cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self

    def _run(self):
        n = 0
        rd = self.proc.stdout
        buf = b""
        while not self._stop.is_set():
            n += 1
            t0 = time.monotonic()
            try:
                self.proc.stdin.write(f"{n}\n".encode())
            except OSError:
                return
            while b"\n" not in buf:
                chunk = rd.read1(4096) if hasattr(rd, "read1") else rd.read(1)
                if not chunk:
                    return
                buf += chunk
            line, buf = buf.split(b"\n", 1)
            t1 = time.monotonic()
            parts = line.decode(errors="replace").split()
            if len(parts) == 2 and parts[0] == str(n):
                tp = float(parts[1])
                mid = (t0 + t1) / 2
                self.pings.append((mid, t1 - t0, tp - mid))
            self._stop.wait(self.period)

    def stop(self):
        self._stop.set()
        if self.proc:
            try:
                self.proc.stdin.close()
            except OSError:
                pass
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        return self.pings


if __name__ == "__main__":
    if sys.argv[1:] == ["serve"]:
        serve()
