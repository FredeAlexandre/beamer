# SPDX-License-Identifier: GPL-3.0-only
"""Tests of the camera E2E analysis on synthetic data (no machine needed).

    uv run --python 3.13 --with numpy dev/camera/e2e/run.py selftest

Besides the pure functions, `SyntheticRunTests` writes a complete fake run folder (meta, clock pings, PC clock
pairs, camprobe dumps/summaries, both events files, status samples) in the shape the live harness collects,
with a PC clock 1000.5 s away from the Mac's and drifting 20 ppm, and runs the real `analyze_dir` on it.
"""
from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import analysis as A  # noqa: E402
import cases as C  # noqa: E402

OFF, PPM, UNIX = 1000.5, 20e-6, 1.79e9   # PC mono = mac + OFF + PPM * mac ; PC unix = PC mono + UNIX


def pc_mono(t_mac):
    return t_mac + OFF + PPM * t_mac


def pc_unix_us(t_mac):
    return int((pc_mono(t_mac) + UNIX) * 1e6)


def pings(t0, t1, rtt=0.004):
    out, t = [], t0
    while t < t1:
        r = rtt * (3 if int(t) % 7 == 0 else 1)
        out.append((t, r, pc_mono(t) - t + (r - rtt) / 2))  # slower pings are skewed: the fit must ignore them
        t += 0.25
    return out


class ClockTests(unittest.TestCase):
    def test_round_trip_and_drift(self):
        c = A.Clock(pings(20000.0, 20600.0))
        for t in (20010.0, 20300.0, 20590.0):
            self.assertAlmostEqual(c.mac_to_pc(t), pc_mono(t), delta=1e-6)
            self.assertAlmostEqual(c.pc_to_mac(pc_mono(t)), t, delta=1e-6)
        self.assertAlmostEqual(c.info["drift_ppm"], 20.0, delta=0.5)

    def test_unix_to_mac(self):
        c = A.Clock(pings(20000.0, 20100.0))
        pairs = [{"mono": pc_mono(t), "unix": pc_mono(t) + UNIX} for t in range(20000, 20100)]
        pt = A.PcTime(c, pairs)
        self.assertAlmostEqual(pt.unix_us(pc_unix_us(20050.0)), 20050.0, delta=2e-6)


class StripTests(unittest.TestCase):
    def test_unwrap_across_the_u32_wrap(self):
        wrap_s = A.TS_WRAP / 1e4                           # 4.97 days
        t = wrap_s * 3 + 0.0123                            # Mac clock just past a wrap
        ts = int(t * 1e4) % A.TS_WRAP
        self.assertAlmostEqual(A.unwrap_ts(ts, t + 0.06), t, delta=1e-4)
        t2 = wrap_s * 3 - 0.02                             # just before it, reference after it
        self.assertAlmostEqual(A.unwrap_ts(int(t2 * 1e4) % A.TS_WRAP, wrap_s * 3 + 0.04), t2, delta=1e-4)


def frames_live(t_from, t_to, lat=0.057, f0=0, period=1 / 30):
    rows, f, t = [], f0, t_from
    while t < t_to:
        rows.append((t, "c1", f, t - lat))
        f += 1
        t += period
    return rows


class FrameTests(unittest.TestCase):
    def test_latency_and_skip(self):
        fr = A.Frames(frames_live(100.0, 110.0))
        lat = A.latency_ms(fr, skip_s=3.0)
        self.assertTrue(all(abs(v - 57.0) < 1e-6 for _, v in lat))
        self.assertGreater(lat[0][0], 102.9)

    def test_frozen_in_counts_a_repeat_as_frozen(self):
        rows = frames_live(100.0, 101.0)
        last = rows[-1]
        rows += [(last[0] + 0.033 * k, "c1", last[2], last[3]) for k in range(1, 9)]  # held picture
        rows += frames_live(last[0] + 0.30, 103.0, f0=last[2] + 10)
        worst, total = A.frozen_in(A.Frames(rows), 100.5, 102.0)
        self.assertAlmostEqual(worst, 300 - A.FRAME_MS, delta=0.2)
        self.assertAlmostEqual(total, worst, delta=0.01)

    def test_integrity(self):
        rows = [(99.9, "other", 0, None), (99.95, "uniform", 0, None)] + frames_live(100.0, 101.0)
        rows.insert(10, (rows[9][0] + 0.001, "c1", 3, rows[9][0] - 0.2))   # an older frame after newer ones
        rows.insert(20, (rows[19][0] + 0.001, "other", 0, None))           # an unreadable strip mid-stream
        integ = A.integrity(A.Frames(rows))
        self.assertEqual(integ["black"], 1)
        self.assertEqual(integ["out_of_order"], 1)
        self.assertEqual(integ["non_live"], 1)
        self.assertEqual(A.corrupt_count(integ), 2)

    def test_start_of_read(self):
        rows = [(50.0 + 0.033 * k, "other", 0, None) for k in range(8)] + frames_live(50.3, 52.0)
        s = A.start_of_read(A.Frames(rows), 50.0)
        self.assertAlmostEqual(s["first_ms"], 300.0, delta=0.5)
        self.assertEqual(s["placeholder_before"], 8)
        self.assertEqual(s["black_before"], 0)


class EventTests(unittest.TestCase):
    def ev(self):
        return [
            {"ev": "stream_state", "stream": 7, "state": "starting", "t": 10.0},
            {"ev": "camera", "stream": 7, "state": "starting", "encoder": "standby", "encoder_ms": 3.1, "t": 10.01},
            {"ev": "stream_state", "stream": 7, "state": "running", "t": 10.2},
            {"ev": "stats", "streams": {"7": {"dir": "send", "kind": "video", "idr": 1, "refresh": 0, "pli_rx": 1}}, "t": 11.0},
            {"ev": "stats", "streams": {"7": {"dir": "send", "kind": "video", "idr": 1, "refresh": 2, "pli_rx": 3},
                                        "8": {"dir": "send", "kind": "mic"}}, "t": 12.0},
            {"ev": "camera", "stream": 7, "state": "stopped", "t": 23.0},
            {"ev": "stream_state", "stream": 7, "state": "stopped", "t": 23.01},
            {"ev": "stream_state", "stream": 8, "state": "starting", "t": 10.0},
        ]

    def test_streams_and_counters(self):
        ss = A.video_send_streams(self.ev())
        self.assertEqual(set(ss), {7})
        self.assertEqual(ss[7]["encoder"], "standby")
        self.assertEqual((ss[7]["on"], ss[7]["off"]), (10.0, 23.0))
        rows = A.video_stats(self.ev(), "send")
        self.assertEqual(A.counter_delta(rows, "refresh", 11.5, 13.0), 2)
        self.assertEqual(A.counter_delta(rows, "pli_rx", 0, 13.0), 3)

    def test_invariant(self):
        ss = A.video_send_streams(self.ev())
        self.assertEqual(A.camera_on_invariant(ss, [(9.9, 19.0)], 30.0), [])           # 4 s after demand: allowed
        bad = A.camera_on_invariant(ss, [(9.9, 15.0)], 30.0)                          # 8 s on without demand
        self.assertEqual(len(bad), 1)
        self.assertAlmostEqual(bad[0][1], 8.0, delta=0.01)
        never_ran = {9: {"stream": 9, "on": 40.0, "off": 43.4}}
        self.assertEqual(A.camera_on_invariant(never_ran, [], 50.0), [])               # 3.4 s, no PLI: allowed
        never_ran[9]["off"] = 44.0
        self.assertEqual(len(A.camera_on_invariant(never_ran, [], 50.0)), 1)


class SyntheticRunTests(unittest.TestCase):
    """A complete fake cam_start run (3 opens) through camlib.Run + cases.an_cam_start + run.analyze_dir."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="cam-e2e-test-"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write_run(self, faults=()):
        import run as R  # noqa: F401  (import check of the entry point)
        spec = C.cam_start(C.Ids(), {"cycles": 3})[0]
        case_dir = self.tmp / "cam_start_test"
        d = case_dir / spec["sub"]
        (d / "pc").mkdir(parents=True)
        t0 = 30000.0
        t_end = t0 + spec["duration"]
        mac_ev, pc_ev, mac_status = [], [], []
        for k, item in enumerate(spec["items"]):
            t_open = t0 + item["at"] + 0.4          # the task start delay
            t_first = t_open + 0.30
            t_close = t_open + item["seconds"]
            rows = [(t_open + 0.1 + 0.033 * j, "other", 0, 0) for j in range(6)]
            if "black" in faults and k == 1:
                rows.append((t_open + 0.29, "uniform", 0, 0))
            for t, c, f, cap in frames_live(t_first, t_close):
                rows.append((t, c, f, int(cap * 1e4) % A.TS_WRAP))
            dump = "".join(f"{pc_unix_us(t)} {c} {f} {ts}\n" for t, c, f, ts in rows)
            (d / "pc" / f"{item['tag']}.dump").write_text(dump)
            summ = {"tag": item["tag"], "ok": True, "type": "NV12 1280x720", "t_begin": pc_unix_us(t_open),
                    "t_first": pc_unix_us(rows[0][0]), "t_stop": pc_unix_us(t_close), "t_closed": pc_unix_us(t_close + 0.02)}
            (d / "pc" / f"{item['tag']}.out").write_text(json.dumps(summ) + "\n")
            sid = 100 + k
            idr = 2 if ("idr" in faults and k == 2) else 1
            mac_ev += [{"ev": "stream_state", "stream": sid, "state": "starting", "mono": t_open + 0.03},
                       {"ev": "camera", "stream": sid, "state": "starting", "encoder": "cold" if k == 0 else "standby", "mono": t_open + 0.031},
                       {"ev": "stream_state", "stream": sid, "state": "running", "mono": t_open + 0.2}]
            mac_ev += [{"ev": "stats", "cpu_pct": 2.0, "mono": t, "streams": {str(sid): {"dir": "send", "kind": "video", "idr": idr}}}
                       for t in (t_open + 1, t_open + 5, t_close - 0.5)]
            stop = t_close + (9.0 if "slow_stop" in faults and k == 0 else 3.2)
            mac_ev += [{"ev": "camera", "stream": sid, "state": "stopped", "mono": stop + 0.01},
                       {"ev": "stream_state", "stream": sid, "state": "stopped", "mono": stop}]
            pc_ev += [{"ev": "vcam", "in_use": True, "mono": pc_mono(t_open + 0.003)},
                      {"ev": "vcam", "in_use": False, "mono": pc_mono(t_close + 0.3)}]
        mac_status = [{"t": t0 + s, "camera": {"warm": {"w": 1280, "h": 720}}} for s in range(0, int(spec["duration"]), 5)]
        meta = {"spec": spec, "opts": {"cycles": 3}, "case": "cam_start", "name": "x", "duration": spec["duration"],
                "t0": t0, "t_end": t_end, "commit": "test", "started": "now"}
        (d / "meta.json").write_text(json.dumps(meta))
        (d / "pings.json").write_text(json.dumps({"pings": pings(t0 - 10, t_end + 10)}))
        pairs = [{"event": "pair", "mono": pc_mono(t), "unix": pc_mono(t) + UNIX} for t in range(int(t0) - 5, int(t_end) + 5)]
        (d / "pc" / "pcside.jsonl").write_text("".join(json.dumps(p) + "\n" for p in pairs))
        (d / "mac_events.jsonl").write_text("".join(json.dumps(e) + "\n" for e in mac_ev))
        (d / "pc" / "pc_events.jsonl").write_text("".join(json.dumps(e) + "\n" for e in pc_ev))
        (d / "mac_status.jsonl").write_text("".join(json.dumps(e) + "\n" for e in mac_status))
        (d / "notes.jsonl").write_text("")
        return case_dir

    def test_clean_run_passes(self):
        import run as R
        res = R.analyze_dir(self.write_run())
        failing = [c for c in res["checks"] if c["pass"] is not True]
        self.assertEqual(res["verdict"], "pass", failing)
        p50 = res["info"]["first_ms"]["p50"]
        self.assertAlmostEqual(p50, 300.0, delta=1.0)
        self.assertTrue((self.tmp / "cam_start_test" / "result.json").exists())

    def test_faults_are_caught(self):
        import run as R
        res = R.analyze_dir(self.write_run(faults=("black", "idr", "slow_stop")))
        bad = {c["name"] for c in res["checks"] if c["pass"] is False}
        self.assertEqual(res["verdict"], "fail")
        self.assertIn("exactly 1 IDR per start (Mac stats)", bad)
        self.assertIn("placeholder before the first picture, never black", bad)
        self.assertIn("Mac camera stopped after the app closed (s, max)", bad)
        self.assertIn("camera-on invariant (§6.5): Mac camera never on without a receiver", bad)


class PlanTests(unittest.TestCase):
    def test_every_case_builds_a_plan(self):
        import run as R
        for name, (builder, *_rest) in C.CASES.items():
            for spec in builder(C.Ids(), {}):
                text = R.plan_text(name, spec)
                self.assertIn(spec["sub"], text)
                reads = sorted((i["at"], i["at"] + i.get("seconds", 0)) for i in spec["items"] if i["kind"] == "read")
                for (a0, a1), (b0, _) in zip(reads, reads[1:]):
                    self.assertLessEqual(a1 + 1.0, b0, f"{name}: app reads overlap (one app at a time)")
                for at, _label, (who, cmd) in spec["actions"]:
                    self.assertIn(who, ("mac", "pc"))
                    self.assertLessEqual(at, spec["duration"], f"{name}: action after the end")
                    json.dumps(cmd)


def fake_case(tmp, name, opts):
    """A generic synthetic run of any case: every read gets a placeholder then the live picture, one Mac stream
    per read (1 IDR, a few refreshes), PC recv/audio stats, vcam demand, status samples, action notes, kills."""
    builder = C.CASES[name][0]
    specs = builder(C.Ids(), opts)
    case_dir = Path(tmp) / f"{name}_fake"
    t0 = 40000.0
    for spec in specs:
        d = case_dir / spec["sub"]
        (d / "pc").mkdir(parents=True)
        t_end = t0 + spec["duration"]
        mac_ev, pc_ev, notes = [], [], []
        sid = 500
        for item in spec["items"]:
            if item["kind"] != "read":
                continue
            sid += 1
            t_open = t0 + item["at"] + 0.4
            t_close = t_open + item["seconds"]
            rows = [(t_open + 0.1 + 0.033 * j, "other", 0, 0) for j in range(5)]
            rows += [(t, c, f, int(cap * 1e4) % A.TS_WRAP) for t, c, f, cap in frames_live(t_open + 0.3, t_close)]
            (d / "pc" / f"{item['tag']}.dump").write_text("".join(f"{pc_unix_us(t)} {c} {f} {ts}\n" for t, c, f, ts in rows))
            (d / "pc" / f"{item['tag']}.out").write_text(json.dumps(
                {"tag": item["tag"], "ok": True, "type": "NV12 " + (item.get("size") or "1280x720"),
                 "t_begin": pc_unix_us(t_open), "t_closed": pc_unix_us(t_close + 0.02)}) + "\n")
            mac_ev += [{"ev": "stream_state", "stream": sid, "state": "starting", "mono": t_open + 0.03},
                       {"ev": "camera", "stream": sid, "state": "starting", "encoder": "standby", "mono": t_open + 0.031},
                       {"ev": "stream_state", "stream": sid, "state": "running", "mono": t_open + 0.2},
                       {"ev": "camera", "stream": sid, "state": "stopped", "mono": t_close + 3.2},
                       {"ev": "stream_state", "stream": sid, "state": "stopped", "mono": t_close + 3.19}]
            pc_ev += [{"ev": "vcam", "in_use": True, "mono": pc_mono(t_open + 0.003)},
                      {"ev": "vcam", "in_use": False, "mono": pc_mono(t_close + 0.3)}]
            t, k = t_open + 1, 0
            while t < t_close:
                k += 1
                mac_ev.append({"ev": "stats", "cpu_pct": 2.0, "mono": t, "streams": {
                    str(sid): {"dir": "send", "kind": "video", "idr": 1, "refresh": k // 5, "pli_rx": k // 5, "kbps": 900,
                               "path": "udp", "dev_drop_au": k // 30}}})
                pc_ev.append({"ev": "stats", "cpu_pct": 1.0, "mono": pc_mono(t), "streams": {
                    str(sid): {"dir": "recv", "kind": "video", "late": k // 10, "lost": 0, "holes": 0, "sync_ms": 0.0,
                               "target_ms": 50.0, "w": 1920 if "1920" in (item.get("size") or "") else 1280,
                               "h": 1080 if "1920" in (item.get("size") or "") else 720, "frozen_ms": 0, "shown": "live",
                               "dec_ms": 2.0, "decoder": "hw", "pli_tx": 1},
                    "9": {"dir": "recv", "kind": "mic", "loss_pct": 0.0, "plc": 0, "underrun": 0}}})
                t += 1.0
        for at, label, _cmd in spec["actions"]:
            notes.append({"event": "action", "at": at, "label": label, "t": t0 + at, "t_done": t0 + at + 0.8})
        pcside = [{"event": "pair", "mono": pc_mono(t), "unix": pc_mono(t) + UNIX} for t in range(int(t0) - 5, int(t_end) + 5)]
        pcside += [{"event": "kill_helper", "mono": pc_mono(t0 + k["at"]), "pid": 1} for k in spec["kills"]]
        for k in spec["kills"]:
            pc_ev.append({"ev": "ready", "mono": pc_mono(t0 + k["at"] + 1.5)})
        st = [{"t": t0 + s_, "camera": {"warm": {"w": 1280}, "refuse": None, "link_out_dropped": 0,
                                        "snapshot": {"status": {"state": "unavailable", "reason": "sharing_off"}}}}
              for s_ in range(0, int(spec["duration"]), 3)]
        meta = {"spec": spec, "opts": opts, "case": name, "name": spec["sub"], "duration": spec["duration"],
                "t0": t0, "t_end": t_end, "commit": "test", "started": "now"}
        files = {"meta.json": json.dumps(meta), "pings.json": json.dumps({"pings": pings(t0 - 10, t_end + 10)}),
                 "mac_events.jsonl": "".join(json.dumps(e) + "\n" for e in mac_ev),
                 "pc/pc_events.jsonl": "".join(json.dumps(e) + "\n" for e in sorted(pc_ev, key=lambda e: e["mono"])),
                 "pc/pcside.jsonl": "".join(json.dumps(e) + "\n" for e in pcside),
                 "mac_status.jsonl": "".join(json.dumps(e) + "\n" for e in st),
                 "pc_status.jsonl": "".join(json.dumps(e) + "\n" for e in st),
                 "notes.jsonl": "".join(json.dumps(e) + "\n" for e in notes),
                 "mac_resources.jsonl": "".join(json.dumps({"t": t0 + 30 * i, "procs": [
                     {"name": "helper", "pid": 1, "cpu": 0.6 * i, "rss": 20e6}, {"name": "VTEncoderXPCService", "pid": 2, "cpu": 0.3 * i, "rss": 30e6}]})
                     + "\n" for i in range(int(spec["duration"] // 30) + 1)),
                 "pc/resources.jsonl": "".join(json.dumps({"mono": pc_mono(t0 + 30 * i), "procs": [
                     {"name": "helper", "pid": 3, "cpu": 0.9 * i, "priv": 5e6}, {"name": "frameserver", "pid": 4, "cpu": 0.2 * i, "priv": 9e6}]})
                     + "\n" for i in range(int(spec["duration"] // 30) + 1))}
        for rel, text in files.items():
            (d / rel).write_text(text)
        t0 = t_end + 60
    return case_dir


class EveryAnalysisTests(unittest.TestCase):
    """Each case's analysis runs to a verdict on a generic synthetic run (no exception, every check well formed)."""

    def test_every_case(self):
        import run as R
        small = {"cycles": 3, "seconds": 40, "reps": 1, "drops": 2}
        tmp = Path(tempfile.mkdtemp(prefix="cam-e2e-all-"))
        try:
            for name in C.CASES:
                with self.subTest(case=name):
                    res = R.analyze_dir(fake_case(tmp, name, dict(small)))
                    self.assertNotIn("traceback", res["info"], res["info"].get("traceback"))
                    self.assertIn(res["verdict"], ("pass", "fail", "incomplete"))
                    for c in res["checks"]:
                        self.assertIn(c["pass"], (True, False, None))
                    json.dumps(res, default=str)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_check_coerces_numpy_bools(self):
        import numpy as np
        self.assertIs(A.check("x", 1, np.bool_(False), "m")["pass"], False)
        self.assertEqual(A.verdict([A.check("x", 1, np.bool_(False), "m")]), "fail")


if __name__ == "__main__":
    unittest.main()
