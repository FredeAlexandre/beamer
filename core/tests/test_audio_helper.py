# SPDX-License-Identifier: GPL-3.0-only
"""HelperProcess against `python -m core.audio.fake_helper`: spawn, commands, EOF, watchdog,
backoff, giving up, Retry, and the stderr rate limit."""

import json
import logging
import os
import queue
import shutil
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

from core.audio import helper as helper_module
from core.audio.helper import HelperProcess

FAKE = [sys.executable, "-m", "core.audio.fake_helper"]


class Events:
    def __init__(self):
        self.queue = queue.Queue()
        self.seen = []

    def __call__(self, event):
        self.queue.put(event)

    def wait(self, test, timeout=10.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                event = self.queue.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            self.seen.append(event)
            if test(event):
                return event
        raise AssertionError(f"no matching event; seen {[e.get('ev') for e in self.seen]}")


def is_ev(name, **fields):
    return lambda event: event.get("ev") == name and all(event.get(key) == value for key, value in fields.items())


class HelperProcessTests(unittest.TestCase):
    def make(self, *extra, **options):
        events = Events()
        options.setdefault("backoff", (0.05, 0.1, 0.2))
        process = HelperProcess(helper_module.command_line(FAKE + list(extra), self.folder), events, **options)
        self.addCleanup(process.stop)
        return process, events

    def setUp(self):
        self.folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.folder)

    def test_spawn_ready_and_commands(self):
        process, events = self.make()
        process.start()
        ready = events.wait(is_ev("ready"))
        self.assertEqual(ready["proto"], 1)
        self.assertEqual(process.state, "ok")
        events.wait(is_ev("devices"))
        self.assertTrue(process.send({"cmd": "flow_open", "req": 7, "flow": "9f2c01aa", "peer": "x", "role": "offer",
                                      "port": 0, "remote": None, "key": "k", "rr_ms": 10}))
        bound = events.wait(is_ev("flow_bound"))
        self.assertEqual(bound["flow"], "9f2c01aa")
        events.wait(is_ev("ok", req=7))
        process.send({"cmd": "dance", "req": 8})
        self.assertEqual(events.wait(is_ev("error", req=8))["code"], "unknown_cmd")
        pid = process.pid
        process.stop()
        self.assertEqual(process.state, "stopped")
        self.assertIsNone(process.pid)
        self.assertFalse(process.send({"cmd": "devices"}))
        self.assertIsNotNone(pid)

    def test_exit_restarts_with_backoff_then_gives_up_and_retry_starts_again(self):
        process, events = self.make("--exit-after", "0.2", fail_limit=3, fail_window=60.0)
        with self.assertLogs("beamer.audio", "INFO") as logs:
            process.start()
            events.wait(is_ev("ready"))
            events.wait(is_ev("_state", state="starting", why="exited"))
            events.wait(is_ev("ready"))
            events.wait(is_ev("_state", state="failed"), timeout=15)
        self.assertEqual(process.state, "failed")
        self.assertTrue(any("giving up" in line for line in logs.output))
        process.retry()
        events.wait(is_ev("_state", state="starting", why="retry"))
        events.wait(is_ev("ready"))

    def test_watchdog_kills_a_helper_that_stops_answering(self):
        process, events = self.make("--no-pong-after", "1", ping_every=0.1, pong_timeout=0.5)
        with self.assertLogs("beamer.audio", "WARNING"):
            process.start()
            events.wait(is_ev("ready"))
            first = process.pid
            events.wait(is_ev("_state", state="starting", why="watchdog"), timeout=10)
            events.wait(is_ev("ready"))
        self.assertNotEqual(process.pid, first)

    def test_missing_binary_counts_as_a_failure(self):
        events = Events()
        process = HelperProcess([os.path.join(self.folder, "nope")], events, backoff=(0.01,), fail_limit=2)
        self.addCleanup(process.stop)
        with self.assertLogs("beamer.audio", "WARNING"):
            process.start()
            events.wait(is_ev("_state", state="failed"))

    def test_stderr_is_rate_limited(self):
        process, events = self.make("--stderr-burst", "400", stderr_rate=50)
        with self.assertLogs("beamer.audio.helper", "DEBUG") as logs:
            process.start()
            events.wait(is_ev("ready"))
            process.stop()
        burst = [line for line in logs.output if "burst line" in line]
        self.assertLessEqual(len(burst), 50)
        self.assertTrue(any("dropped" in line for line in logs.output))

    def test_eof_on_stdin_ends_the_helper(self):
        process, events = self.make()
        process.start()
        events.wait(is_ev("ready"))
        proc = process._proc
        process.stop(timeout=3)
        self.assertEqual(proc.returncode, 0)


class LocateTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.folder)

    def test_order_dev_switch_env_build(self):
        binary = Path(self.folder, "beamer-audio-null")
        binary.write_text("")
        with mock.patch.dict(os.environ, {helper_module.ENV_HELPER: str(binary)}):
            self.assertEqual(helper_module.locate(self.folder), [str(binary)])
            Path(self.folder, helper_module.DEV_ENABLED).write_text(json.dumps({"helper": ["/x/y", "--backend", "null"]}))
            self.assertEqual(helper_module.locate(self.folder), ["/x/y", "--backend", "null"])
        Path(self.folder, helper_module.DEV_ENABLED).write_text("")
        self.assertEqual(helper_module.dev_config(self.folder), {})
        with mock.patch.dict(os.environ, {helper_module.ENV_HELPER: os.path.join(self.folder, "missing")}):
            with mock.patch.object(helper_module, "REPO", Path(self.folder)):
                self.assertIsNone(helper_module.locate(self.folder))

    def test_available_needs_the_flag_and_a_file(self):
        binary = Path(self.folder, "helper")
        binary.write_text("")
        with mock.patch.dict(os.environ, {helper_module.ENV_HELPER: str(binary)}):
            with mock.patch.object(helper_module.feature_flags, "AUDIO", True, create=True):
                self.assertTrue(helper_module.available())
            with mock.patch.object(helper_module.feature_flags, "AUDIO", False, create=True):
                self.assertFalse(helper_module.available())

    def test_command_line(self):
        argv = helper_module.command_line(["beamer-audio"], "/state", dev=True)
        self.assertEqual(argv[:2], ["beamer-audio", "--control"])
        self.assertIn("--dev", argv)
        self.assertEqual(argv[argv.index("--exclude-pid") + 1], str(os.getpid()))
        self.assertEqual(argv[argv.index("--state-dir") + 1], "/state")


if __name__ == "__main__":
    unittest.main()
