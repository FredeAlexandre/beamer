# SPDX-License-Identifier: GPL-3.0-only
"""The native audio helper as a child process (dev/audio/PHASE2.md §3, §8.2).

`HelperProcess` spawns `beamer-audio --control ...`, writes JSON-line commands to its stdin, turns its
stdout into events for the AudioService and logs its stderr. A watchdog pings it every 5 s; no `pong`
within 10 s, or its stdout ending, kills it and starts it again after 1, 2, 4, 8, 16, 30 s; five
failures within two minutes leave it `failed` until `retry()`.

Besides the helper's own events (§3.3), `deliver` gets `{"ev": "_state", "state": "starting" |
"failed", "why": ...}` when the process (re)starts or gives up. A `ready` event means it is up.
"""

import json
import logging
import os
import queue
import subprocess
import sys
import threading
import time
from pathlib import Path

from core import feature_flags

log = logging.getLogger("beamer.audio")
helper_log = logging.getLogger("beamer.audio.helper")

CREATE_NO_WINDOW = 0x08000000
MAX_LINE = 64 * 1024
DEV_ENABLED = "audio-dev.enabled"
ENV_HELPER = "BEAMER_AUDIO_HELPER"
REPO = Path(__file__).resolve().parents[2]


def binary_name(platform=None):
    platform = sys.platform if platform is None else platform
    return "beamer-audio.exe" if platform.startswith("win") else "beamer-audio"


def dev_config(state_dir):
    """The dev switch's contents: None when `<state_dir>/audio-dev.enabled` does not exist, else a dict
    (empty when the file is empty or not JSON)."""
    path = Path(state_dir) / DEV_ENABLED
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return None
    except OSError:
        return {}
    try:
        data = json.loads(text) if text.strip() else {}
    except ValueError:
        return {}
    return data if isinstance(data, dict) else {}


def locate(state_dir=None):
    """The helper's argv before `--control`: the dev switch's `helper`, else $BEAMER_AUDIO_HELPER, else
    next to the frozen app, else the repository's build output. None when no file exists."""
    dev = dev_config(state_dir) if state_dir is not None else None
    helper = (dev or {}).get("helper")
    if isinstance(helper, list) and helper and all(isinstance(part, str) for part in helper):
        return list(helper)
    if isinstance(helper, str) and helper:
        return [helper]
    candidates = []
    if os.environ.get(ENV_HELPER):
        candidates.append(Path(os.environ[ENV_HELPER]))
    if getattr(sys, "frozen", False):
        candidates.append(Path(sys.executable).parent / binary_name())
    system = "win" if sys.platform.startswith("win") else "mac"
    candidates.append(REPO / "native" / "audio" / "build" / system / binary_name())
    for candidate in candidates:
        if candidate.is_file():
            return [str(candidate)]
    return None


def available(state_dir=None):
    """Whether this machine can advertise `audio`: the feature flag is on and a helper exists."""
    return bool(getattr(feature_flags, "AUDIO", False)) and locate(state_dir) is not None


def command_line(base, state_dir, *, parent_pid=None, exclude_pid=None, dev=False, log_level="info"):
    pid = os.getpid()
    argv = list(base) + ["--control", "--parent-pid", str(parent_pid or pid), "--state-dir", str(state_dir),
                         "--exclude-pid", str(exclude_pid or pid), "--log-level", log_level]
    if dev:
        argv.append("--dev")
    return argv


_LEVELS = {"debug": logging.DEBUG, "info": logging.INFO, "warn": logging.WARNING, "warning": logging.WARNING,
           "error": logging.ERROR}


class HelperProcess:
    """One helper child at a time, restarted with backoff. Thread-safe `send`."""

    def __init__(self, cmd, deliver, *, ping_every=5.0, pong_timeout=10.0, backoff=(1, 2, 4, 8, 16, 30),
                 fail_window=120.0, fail_limit=5, stderr_rate=50, clock=time.monotonic,
                 popen=subprocess.Popen, logger=None):
        self.cmd = list(cmd)
        self._deliver = deliver
        self._ping_every = ping_every
        self._pong_timeout = pong_timeout
        self._backoff = tuple(backoff)
        self._fail_window = fail_window
        self._fail_limit = fail_limit
        self._stderr_rate = stderr_rate
        self._clock = clock
        self._popen = popen
        self._log = logger or log
        self._write_lock = threading.Lock()
        self._inbox = queue.Queue()
        self._proc = None
        self._generation = 0
        self._failures = []
        self._state = "stopped"
        self._stopping = False
        self._thread = None
        self._ping_req = 1 << 30
        self._readers = []

    @property
    def state(self):
        return self._state

    @property
    def pid(self):
        proc = self._proc
        return proc.pid if proc is not None else None

    def start(self):
        if self._thread is not None:
            return
        self._stopping = False
        self._thread = threading.Thread(target=self._supervise, name="Beamer-audio-helper", daemon=True)
        self._thread.start()

    def send(self, command):
        """Write one command line; False when no helper is running or the pipe broke."""
        proc = self._proc
        if proc is None or proc.stdin is None:
            return False
        line = (json.dumps(command, separators=(",", ":")) + "\n").encode("utf-8")
        if len(line) >= MAX_LINE:
            self._log.warning("audio: command too long, not sent: %s", command.get("cmd"))
            return False
        with self._write_lock:
            try:
                proc.stdin.write(line)
                proc.stdin.flush()
            except (OSError, ValueError):
                return False
        return True

    def retry(self):
        self._inbox.put(("retry", None))

    def stop(self, timeout=2.0):
        self._stopping = True
        self._inbox.put(("stop", None))
        thread, self._thread = self._thread, None
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout + 1.0)
        self._end(timeout)
        self._state = "stopped"

    # Supervisor thread ----------------------------------------------------------------------------

    def _set_state(self, state, why=""):
        self._state = state
        if state in ("starting", "failed"):
            self._deliver({"ev": "_state", "state": state, "why": why})

    def _spawn(self):
        self._generation += 1
        generation = self._generation
        kwargs = dict(stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        if sys.platform.startswith("win"):
            kwargs["creationflags"] = CREATE_NO_WINDOW
        try:
            proc = self._popen(self.cmd, **kwargs)
        except (OSError, ValueError) as error:
            self._log.warning("audio: helper did not start: %s", error)
            return False
        self._proc = proc
        self._log.info("audio: helper started, pid %s", proc.pid)
        self._readers = [
            threading.Thread(target=self._read_stdout, args=(proc, generation), name="Beamer-audio-out", daemon=True),
            threading.Thread(target=self._read_stderr, args=(proc,), name="Beamer-audio-err", daemon=True)]
        for reader in self._readers:
            reader.start()
        return True

    def _reap(self, proc):
        """After the child exited: let its readers reach EOF, then close the pipes."""
        for reader in self._readers:
            if reader is not threading.current_thread():
                reader.join(1.0)
        for pipe in (proc.stdin, proc.stdout, proc.stderr):
            try:
                if pipe is not None:
                    pipe.close()
            except OSError:
                pass

    def _end(self, timeout=1.0):
        proc, self._proc = self._proc, None
        if proc is None:
            return
        try:
            with self._write_lock:
                if proc.stdin is not None:
                    proc.stdin.write(b'{"cmd":"quit"}\n')
                    proc.stdin.flush()
                    proc.stdin.close()
        except (OSError, ValueError):
            pass
        try:
            proc.wait(timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            try:
                proc.wait(1.0)
            except subprocess.TimeoutExpired:
                pass
        self._reap(proc)

    def _kill(self):
        proc, self._proc = self._proc, None
        if proc is None:
            return
        try:
            proc.kill()
            proc.wait(2.0)
        except (OSError, subprocess.TimeoutExpired):
            pass
        self._reap(proc)

    def _failed_lately(self, now):
        self._failures = [moment for moment in self._failures if now - moment < self._fail_window]
        return len(self._failures)

    def _supervise(self):
        restart_at = self._clock()
        self._set_state("starting", "start")
        last_pong = last_ping = 0.0
        while True:
            now = self._clock()
            if self._proc is None and restart_at is not None and now >= restart_at:
                restart_at = None
                if self._spawn():
                    last_pong = last_ping = self._clock()
                else:
                    restart_at = self._failure("spawn")
                continue
            if self._proc is not None:
                if now - last_pong > self._pong_timeout:
                    self._log.warning("audio: helper answered no ping for %.0f s, killing it", now - last_pong)
                    self._kill()
                    restart_at = self._failure("watchdog")
                    continue
                if now - last_ping >= self._ping_every:
                    last_ping = now
                    self._ping_req += 1
                    self.send({"cmd": "ping", "req": self._ping_req})
            deadlines = [last_ping + self._ping_every, last_pong + self._pong_timeout] if self._proc else []
            if restart_at is not None:
                deadlines.append(restart_at)
            wait = max(0.0, min(deadlines) - self._clock()) if deadlines else None
            try:
                kind, value = self._inbox.get(timeout=wait)
            except queue.Empty:
                continue
            if kind == "stop":
                return
            if kind == "pong" and value == self._generation:
                last_pong = self._clock()
            elif kind == "exit" and value == self._generation and self._proc is not None:
                self._kill()
                restart_at = self._failure("exited")
            elif kind == "retry" and self._state == "failed":
                self._failures.clear()
                self._set_state("starting", "retry")
                restart_at = self._clock()

    def _failure(self, why):
        """Record a failure; the next start time, or None when the helper is given up as failed."""
        if self._stopping:
            return None
        now = self._clock()
        self._failures.append(now)
        count = self._failed_lately(now)
        if count >= self._fail_limit:
            self._log.warning("audio: helper failed %d times in %.0f s, giving up until Retry", count,
                              self._fail_window)
            self._set_state("failed", why)
            return None
        delay = self._backoff[min(count - 1, len(self._backoff) - 1)]
        self._log.info("audio: helper %s, restarting in %s s", why, delay)
        self._set_state("starting", why)
        return now + delay

    def _read_stdout(self, proc, generation):
        stream = proc.stdout
        try:
            for raw in iter(stream.readline, b""):
                try:
                    event = json.loads(raw.decode("utf-8"))
                except (UnicodeDecodeError, ValueError):
                    self._log.debug("audio: helper wrote a line that is not JSON")
                    continue
                if not isinstance(event, dict) or not isinstance(event.get("ev"), str):
                    continue
                if generation != self._generation:
                    return
                if event["ev"] == "pong":
                    self._inbox.put(("pong", generation))
                    continue
                if event["ev"] == "ready":
                    self._state = "ok"
                self._deliver(event)
        except (OSError, ValueError):
            pass
        finally:
            self._inbox.put(("exit", generation))

    def _read_stderr(self, proc):
        window, count, dropped = 0.0, 0, 0
        try:
            for raw in iter(proc.stderr.readline, b""):
                now = self._clock()
                if now - window >= 1.0:
                    if dropped:
                        helper_log.warning("%d helper log lines dropped (more than %d/s)", dropped, self._stderr_rate)
                    window, count, dropped = now, 0, 0
                count += 1
                if count > self._stderr_rate:
                    dropped += 1
                    continue
                text = raw.decode("utf-8", "replace").rstrip()
                level, _colon, rest = text.partition(": ")
                if level.lower() in _LEVELS and rest:
                    helper_log.log(_LEVELS[level.lower()], "%s", rest)
                else:
                    helper_log.info("%s", text)
        except (OSError, ValueError):
            pass
        if dropped:
            helper_log.warning("%d helper log lines dropped (more than %d/s)", dropped, self._stderr_rate)
