# SPDX-License-Identifier: GPL-3.0-only
"""core.camera.wire: builders, validators, limits, unknown fields, `camera1` gating (PHASE2 §5, §10.1)."""

import base64
import copy
import unittest

from core.camera import model, wire
from core.camera.model import Camera, CameraEntry, CameraRef, CameraRoute, MachineCamera, QualityEntry, Stamp

MAC = "AQEBAQEBAQEBAQEBAQEBAQ"
PC = "AgICAgICAgICAgICAgICAg"
NOW = 1793000000.0
KEY = base64.b64encode(bytes(32)).decode()


def mac_list(**changes):
    machine = MachineCamera(
        id=MAC, platform="macos", rev=5, status="ok", features=frozenset({"camera_capture"}),
        cameras=(Camera("cam-1", "MacBook Air Camera", "builtin", ("1920x1080@30", "1280x720@30"), True),),
        preferred="cam-1", permission="granted", sharing=True,
        capture=model.Capture("running", False, ("center_stage",), None), virtual_camera=model.NO_VIRTUAL_CAMERA)
    return MachineCamera(**{**machine.__dict__, **changes})


def pc_list(**changes):
    machine = MachineCamera(
        id=PC, platform="windows", rev=7, status="ok", features=frozenset({"virtual_camera"}), cameras=(),
        preferred=None, permission=None, sharing=None, capture=model.IDLE_CAPTURE,
        virtual_camera=model.VirtualCamera("ok", True, True, "Microsoft Teams", (1280, 720, 30)))
    return MachineCamera(**{**machine.__dict__, **changes})


def read(msg):
    return wire.read(msg, now=NOW)


class DevicesTests(unittest.TestCase):
    def test_round_trip(self):
        for machine in (mac_list(), pc_list(), mac_list(test_source="synth:1920x1440;start_ms=800")):
            self.assertEqual(read(wire.devices(machine)), (wire.DEVICES, machine))

    def test_unknown_fields_ignored_and_unknown_values_defaulted(self):
        msg = wire.devices(mac_list())
        msg["data"]["future"] = {"x": 1}
        msg["data"]["cameras"][0]["extra"] = 3
        msg["data"]["cameras"][0]["transport"] = "wormhole"
        msg["data"]["cameras"][0]["native"].append("garbage")
        msg["data"]["capture"]["effects"].append("sparkles")
        msg["data"]["features"].append("hologram")
        _kind, machine = read(msg)
        self.assertEqual(machine.cameras[0].transport, "other")
        self.assertEqual(machine.cameras[0].native, ("1920x1080@30", "1280x720@30"))
        self.assertEqual(machine.capture.effects, ("center_stage",))
        self.assertEqual(machine.features, frozenset({"camera_capture"}))

    def test_limits(self):
        base = wire.devices(mac_list())
        many = copy.deepcopy(base)
        many["data"]["cameras"] = [{"key": f"k{i}", "name": f"n{i}"} for i in range(wire.MAX_CAMERAS + 1)]
        self.assertIsNone(read(many))
        native = copy.deepcopy(base)
        native["data"]["cameras"][0]["native"] = ["1280x720@30"] * (wire.MAX_NATIVE + 1)
        self.assertIsNone(read(native))
        for field, value in (("key", "k" * (wire.MAX_KEY + 1)), ("name", "n" * (wire.MAX_NAME + 1)), ("key", "")):
            bad = copy.deepcopy(base)
            bad["data"]["cameras"][0][field] = value
            self.assertIsNone(read(bad), field)
        app = wire.devices(pc_list())
        app["data"]["virtual_camera"]["app"] = "a" * (wire.MAX_APP + 1)
        self.assertIsNone(read(app))

    def test_malformed_is_ignored(self):
        base = wire.devices(mac_list())
        for change in ({"machine": "nope"}, {"rev": -1}, {"rev": "1"}, {"cameras": "x"}, {"sharing": "yes"},
                       {"preferred": 3}, {"capture": []}, {"virtual_camera": 1}, {"features": "camera_capture"}):
            bad = copy.deepcopy(base)
            bad["data"].update(change)
            self.assertIsNone(read(bad), change)
        self.assertIsNone(read({"type": wire.DEVICES, "data": None}))
        self.assertIsNone(read({"type": "camera_bogus", "data": {}}))
        self.assertIsNone(read({"type": "audio_devices", "data": {}}))
        bad = wire.devices(pc_list())
        bad["data"]["virtual_camera"]["want"] = {"w": "x"}
        self.assertIsNone(read(bad))

    def test_sharing_and_test_source_only_from_a_capture_machine(self):
        msg = wire.devices(pc_list())
        msg["data"]["sharing"] = False
        msg["data"]["test_source"] = "synth"
        _kind, machine = read(msg)
        self.assertIsNone(machine.sharing)
        self.assertIsNone(machine.test_source)
        missing = wire.devices(mac_list())
        missing["data"]["sharing"] = None
        self.assertIs(read(missing)[1].sharing, True)
        bad_source = wire.devices(mac_list())
        bad_source["data"]["test_source"] = "rm -rf"
        self.assertIsNone(read(bad_source)[1].test_source)

    def test_size_limit(self):
        self.assertTrue(wire.size_ok(wire.devices(mac_list())))
        self.assertFalse(wire.size_ok({"type": wire.DEVICES, "data": {"x": "y" * wire.MAX_SIZE}}))


class RouteTests(unittest.TestCase):
    def route(self, **extra):
        return CameraRoute(CameraEntry(MAC, CameraRef("cam-1", "MacBook Air Camera"), Stamp(1793000000, PC)),
                           QualityEntry("1080p", Stamp(1793000001, MAC)), extra)

    def test_round_trip_with_unknown_entries_kept(self):
        value = self.route(sinks={PC: {"from": MAC}, "set_at": 3, "by": PC})
        self.assertEqual(read(wire.route(value)), (wire.ROUTE, value))

    def test_off_route_drops_a_device(self):
        msg = wire.route(self.route())
        msg["data"]["camera"]["machine"] = None
        self.assertIsNone(read(msg)[1].camera.device)

    def test_malformed(self):
        for change in ({"camera": None}, {"quality": {"size": "4k", "set_at": 1, "by": MAC}},
                       {"camera": {"machine": "x", "set_at": 1, "by": MAC}},
                       {"camera": {"machine": MAC, "device": {"key": ""}, "set_at": 1, "by": MAC}}):
            msg = wire.route(self.route())
            msg["data"].update(change)
            self.assertIsNone(read(msg), change)

    def test_bad_entries_are_dropped_one_by_one(self):
        """R4 #1: an entry with a bad stamp (or too far ahead), or an unknown entry that is not a small
        object, is dropped on its own; the held entry stands in for it, so the merge neither takes it
        nor answers about it."""
        held = self.route(kept={"v": 1, "set_at": 5, "by": MAC})
        msg = wire.route(self.route())
        msg["data"]["camera"]["set_at"] = int(NOW) + wire.ROUTE_FUTURE + 5
        msg["data"].update({
            "future": {"set_at": 2 ** 53, "by": ""}, "ahead": {"set_at": int(NOW) + 2 * 86400},
            "blob": {"data": "x" * 17_000}, "number": 3, "kept": {"v": 2, "set_at": 2 ** 60},
            "fine": {"v": 1, "set_at": int(NOW), "by": PC}, "plain": {"v": 1}})
        route, dropped = wire.check_route(msg["data"], now=NOW, held=held)
        self.assertEqual(set(dropped), {"camera", "future", "ahead", "blob", "number", "kept"})
        self.assertEqual(route.camera, held.camera)
        self.assertEqual(route.quality, self.route().quality)
        self.assertEqual(route.extra, {"kept": held.extra["kept"], "fine": msg["data"]["fine"], "plain": {"v": 1}})
        merged, changed, answer = model.merge_route(held, route)
        self.assertEqual(merged.camera, held.camera)
        # Without a held route (camera.json, the generic reader) a dropped camera entry is "never set".
        self.assertEqual(read(msg)[1].camera, model.CAMERA_OFF)
        self.assertEqual(model.max_stamp(route), max(route.camera.stamp.set_at, route.quality.stamp.set_at, int(NOW)))

    def test_at_most_max_extra_unknown_entries(self):
        held = self.route(**{f"h{i}": {"v": i} for i in range(wire.MAX_EXTRA - 2)})
        msg = wire.route(self.route())
        msg["data"].update({f"x{i}": {"v": i} for i in range(5)})
        route, dropped = wire.check_route(msg["data"], now=NOW, held=held)
        self.assertEqual(sorted(route.extra), ["x0", "x1"])
        self.assertEqual(dropped, ("x2", "x3", "x4"))
        merged, _changed, _answer = model.merge_route(held, route)
        self.assertEqual(len(merged.extra), wire.MAX_EXTRA)
        self.assertTrue(wire.size_ok(wire.route(merged)))

    def test_missing_quality_is_the_default(self):
        msg = wire.route(self.route())
        del msg["data"]["quality"]
        self.assertEqual(read(msg)[1].quality, model.QUALITY_DEFAULT)


class StreamMessageTests(unittest.TestCase):
    def test_open_round_trip(self):
        codec = wire.codec(1280, 720, 2_500_000)
        msg = wire.open_stream(1288, "9f2c01aa", codec, KEY, CameraRef("cam-1", "MacBook Air Camera"),
                               Stamp(1793000000, MAC))
        kind, data = read(msg)
        self.assertEqual(kind, wire.OPEN)
        self.assertEqual(data["codec"], codec)
        self.assertEqual(data["camera"], CameraRef("cam-1", "MacBook Air Camera"))
        self.assertEqual(data["for"], Stamp(1793000000, MAC))
        self.assertEqual(data["stream"], 1288)
        self.assertIsNone(read(wire.open_stream(1, "9f2c01aa", codec, KEY, None, Stamp(1, MAC)))[1]["camera"])

    def test_open_malformed(self):
        codec = wire.codec(1280, 720, 2_500_000)
        good = wire.open_stream(5, "9f2c01aa", codec, KEY, None, Stamp(1, MAC))
        for field, value in (("stream", 0), ("flow", "XYZ"), ("key", "short"), ("for", None),
                             ("codec", dict(codec, name="vp8")), ("codec", dict(codec, w=1281)),
                             ("codec", dict(codec, h=4000)), ("codec", dict(codec, fps=60)),
                             ("codec", dict(codec, bitrate=1)), ("camera", {"key": 1})):
            bad = copy.deepcopy(good)
            bad["data"][field] = value
            self.assertIsNone(read(bad), field)

    def test_opened_and_close(self):
        self.assertEqual(read(wire.opened(5, True))[1], {"stream": 5, "ok": True, "error": None})
        self.assertEqual(read(wire.opened(5, False, "not_source"))[1]["error"], "not_source")
        self.assertEqual(read(wire.opened(5, False, "weird"))[1]["error"], "unsupported")
        self.assertEqual(read(wire.close(5, "no_receiver"))[1]["reason"], "no_receiver")
        self.assertEqual(read(wire.close(5, "weird"))[1]["reason"], "error")
        self.assertIsNone(read({"type": wire.OPENED, "data": {"stream": 5}}))
        self.assertIsNone(read({"type": wire.CLOSE, "data": {"stream": "5"}}))


class GatingTests(unittest.TestCase):
    def test_may_send_needs_audio_and_camera1(self):
        self.assertTrue(wire.may_send({"audio", "text"}, {"camera1", "video1"}))
        self.assertFalse(wire.may_send({"text"}, {"camera1"}))
        self.assertFalse(wire.may_send({"audio"}, {"video1"}))
        self.assertFalse(wire.may_send(None, None))

    def test_announce_needs_the_flag_and_video1(self):
        self.assertTrue(wire.announce(True, ["video1", "camera_capture"]))
        self.assertFalse(wire.announce(False, ["video1"]))
        self.assertFalse(wire.announce(True, ["camera_capture"]))

    def test_is_camera(self):
        self.assertTrue(wire.is_camera({"type": "camera_route"}))
        self.assertFalse(wire.is_camera({"type": "audio_route"}))
        self.assertFalse(wire.is_camera("camera_route"))

    def test_test_source_specs(self):
        for good in ("synth", "synth:1920x1440", "synth;start_ms=800", "synth:1280x720;start_ms=800"):
            self.assertTrue(wire.test_source_ok(good), good)
        for bad in ("", "synth:", "synthx", "camera", "synth;start_ms=", None, 3):
            self.assertFalse(wire.test_source_ok(bad), bad)


if __name__ == "__main__":
    unittest.main()
