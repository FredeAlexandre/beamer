# SPDX-License-Identifier: GPL-3.0-only
"""The audio_* link messages: builders, validators, limits; malformed input is ignored."""

import base64
import copy
import unittest

from core import protocol, settings_sync
from core.audio import model, wire
from core.audio.model import DeviceRef, FormEntry, MicRoute, OutputRoute, Route, Stamp

NOW = 1793000000


def mid(seed):
    return protocol.id_text(bytes([seed]) * 16)


A, B = mid(1), mid(2)
KEY = base64.b64encode(bytes(range(32))).decode()


def device(index, kind="input"):
    return model.Device(f"key-{index}", f"Device {index}", kind, "usb", 2, "speakers" if kind == "output" else "microphone",
                        index == 0)


def machine(**changes):
    values = dict(id=A, name="mac", platform="macos", rev=NOW * 1000, status="ok", inputs=(device(0),),
                  outputs=(device(0, "output"), device(1, "output")), default_input="key-0", default_output="key-0",
                  virtual_mic=model.VirtualMic("beamer", True, False), features=frozenset({"aec", "mic_demand"}),
                  links=frozenset({B}), helper="0.1.0")
    values.update(changes)
    return model.MachineAudio(**values)


def route():
    return Route(MicRoute(A, DeviceRef("key-0", "Device 0"), Stamp(NOW, A)),
                 {B: OutputRoute(A, None, Stamp(NOW - 5, B))},
                 {(A, "key-0"): FormEntry("headset", Stamp(NOW - 9, A))})


class RoundTripTests(unittest.TestCase):
    def check(self, msg):
        self.assertTrue(wire.size_ok(msg))
        read = wire.read(msg, now=NOW)
        self.assertIsNotNone(read, msg)
        self.assertEqual(read[0], msg["type"])
        return read[1]

    def test_every_builder_reads_back(self):
        hello = self.check(wire.hello("macos", "ok", False, ["aec"]))
        self.assertEqual((hello["v"], hello["min_v"], hello["listen"]), (1, 1, False))
        self.assertEqual(self.check(wire.devices(machine())), machine())
        self.assertEqual(self.check(wire.route(route())), route())
        self.assertEqual(self.check(wire.flow_offer("9f2c01aa", 52114, KEY))["key"], KEY)
        self.assertEqual(self.check(wire.flow_answer("9f2c01aa", 24823))["port"], 24823)
        self.assertEqual(self.check(wire.flow_want()), {"phase": "want"})
        self.assertEqual(self.check(wire.flow_close("9f2c01aa"))["phase"], "close")
        opened = self.check(wire.open_stream(7, "9f2c01aa", "mic", wire.CODECS["mic"], KEY, {"virtual_mic": True},
                                             Stamp(NOW, A)))
        self.assertEqual((opened["stream"], opened["for"]), (7, Stamp(NOW, A)))
        system = self.check(wire.open_stream(8, "9f2c01aa", "system", wire.CODECS["system"], KEY,
                                             {"device": {"key": "k", "name": "n"}}, Stamp(NOW, A)))
        self.assertEqual(system["sink"], {"device": DeviceRef("k", "n")})
        self.assertEqual(self.check(wire.opened(7, False, "no_device"))["error"], "no_device")
        self.assertEqual(self.check(wire.opened(7, True))["error"], None)
        self.assertEqual(self.check(wire.close(7, "route"))["reason"], "route")
        self.assertEqual(self.check(wire.frame("9f2c01aa", ["AAAA"] * 4))["p"], ["AAAA"] * 4)
        self.assertEqual(self.check(wire.watch(True)), {"on": True})
        stats = self.check(wire.stats(5, {"in:k": [-20.0, -6.0]}, {"7": {"path": "udp", "jb_ms": 23.0}}))
        self.assertEqual(stats["levels"], {("in", "k"): (-20.0, -6.0)})
        self.assertEqual(stats["streams"][7]["jb_ms"], 23.0)
        self.assertEqual(self.check(wire.test("k", 10)), {"device": "k", "seconds": 10})

    def test_full_device_lists_fit(self):
        many = machine(inputs=tuple(model.Device("k" * 199 + str(i % 10), "n" * 128, "input", "usb", 2, "microphone", False)
                                    for i in range(32)),
                       outputs=tuple(model.Device("o" * 199 + str(i % 10), "n" * 128, "output", "usb", 2, "speakers", False)
                                     for i in range(32)),
                       links=frozenset(mid(i) for i in range(1, 34)))
        self.assertTrue(wire.size_ok(wire.devices(many)))


class MalformedTests(unittest.TestCase):
    def assert_ignored(self, msg):
        self.assertIsNone(wire.read(msg, now=NOW), msg)

    def test_not_audio_or_unknown(self):
        self.assert_ignored({"type": "settings", "data": {}})
        self.assert_ignored({"type": "audio_future", "data": {}})
        self.assert_ignored({"type": "audio_hello", "data": "nope"})
        self.assert_ignored("audio_hello")
        self.assert_ignored({"type": "audio_hello"})

    def test_hello(self):
        good = wire.hello("windows", "ok", True)
        for field, value in (("v", 0), ("min_v", "1"), ("udp", {"listen": "yes"}), ("platform", ""), ("udp", None)):
            bad = copy.deepcopy(good)
            bad["data"][field] = value
            self.assert_ignored(bad)
        unknown = copy.deepcopy(good)
        unknown["data"]["helper"] = "weird"
        unknown["data"]["extra"] = {"x": 1}
        self.assertEqual(wire.read(unknown, now=NOW)[1]["helper"], "failed")

    def test_devices_limits(self):
        def bad(**changes):
            msg = wire.devices(machine())
            msg["data"].update(changes)
            self.assert_ignored(msg)
        bad(machine="not-an-id")
        bad(rev=-1)
        bad(rev="1")
        bad(rev=(NOW + 2 * settings_sync.DAY) * 1000)
        bad(inputs=[{"key": "k", "name": "n"}] * 33)
        bad(inputs=[{"key": "k" * 201, "name": "n"}])
        bad(inputs=[{"key": "k", "name": "n" * 129}])
        bad(inputs=[{"key": "", "name": "n"}])
        bad(default_input=5)
        bad(links=["nope"])
        bad(links=[mid(i) for i in range(1, 35)])
        bad(virtual_mic="x")
        odd = wire.devices(machine())
        odd["data"]["inputs"] = [{"key": "k", "name": "n", "form": "toaster", "transport": "carrier pigeon",
                                  "channels": 900}]
        read = wire.read(odd, now=NOW)[1]
        self.assertEqual((read.inputs[0].form, read.inputs[0].transport, read.inputs[0].channels),
                         ("unknown", "other", 0))

    def test_route_limits(self):
        def bad(mutate):
            msg = wire.route(route())
            mutate(msg["data"])
            self.assert_ignored(msg)
        bad(lambda data: data["mic"].update(set_at=NOW + 2 * settings_sync.DAY))
        bad(lambda data: data["mic"].update(set_at=2 ** 53))
        bad(lambda data: data["mic"].update(by="bad"))
        bad(lambda data: data["mic"].update(machine="bad"))
        bad(lambda data: data["mic"].update(device={"key": "k"}))
        bad(lambda data: data["outputs"][B].update(to=B))
        bad(lambda data: data["outputs"].update({mid(i): {"to": None, "device": None, "set_at": 1, "by": A}
                                                for i in range(3, 40)}))
        bad(lambda data: data["forms"].update({"x y": {"form": None, "set_at": 1, "by": A}}))
        bad(lambda data: data["forms"][f"{A} key-0"].update(form="earbuds"))
        bad(lambda data: data.update(mic=None))

    def test_forms_over_the_limit_are_trimmed(self):
        msg = wire.route(route())
        msg["data"]["forms"] = {f"{A} k{i}": {"form": None if i < 20 else "headset", "set_at": i + 1, "by": A}
                                for i in range(80)}
        read = wire.read(msg, now=NOW)[1]
        self.assertEqual(len(read.forms), model.MAX_FORMS)
        self.assertNotIn((A, "k0"), read.forms)

    def test_flow_open_frame(self):
        self.assert_ignored(wire.flow_offer("9F2C01AA", 1, KEY))
        self.assert_ignored(wire.flow_offer("9f2c01aa", 70000, KEY))
        self.assert_ignored(wire.flow_offer("9f2c01aa", 1, base64.b64encode(bytes(31)).decode()))
        self.assert_ignored(wire.flow_offer("9f2c01aa", 1, "!!!"))
        self.assert_ignored({"type": wire.FLOW, "data": {"phase": "dance"}})
        good = wire.open_stream(7, "9f2c01aa", "mic", wire.CODECS["mic"], KEY, {"virtual_mic": True}, Stamp(NOW, A))
        for mutate in (lambda d: d.update(stream=0), lambda d: d.update(stream=2 ** 31), lambda d: d.update(kind="video"),
                       lambda d: d["codec"].update(name="aac"), lambda d: d["codec"].update(bitrate=10 ** 7),
                       lambda d: d.update(sink={}), lambda d: d.update(sink={"device": {"key": 3, "name": "x"}}),
                       lambda d: d.update(**{"for": {"set_at": "x", "by": A}}), lambda d: d.pop("key")):
            bad = copy.deepcopy(good)
            mutate(bad["data"])
            self.assert_ignored(bad)
        self.assert_ignored(wire.frame("9f2c01aa", ["AAAA"] * 5))
        self.assert_ignored(wire.frame("9f2c01aa", []))
        self.assert_ignored(wire.frame("9f2c01aa", [3]))
        self.assert_ignored(wire.opened(0, True))
        self.assert_ignored({"type": wire.WATCH, "data": {"on": "yes"}})
        self.assert_ignored(wire.test("", 3))
        self.assertEqual(wire.read(wire.close(5, "bored"), now=NOW)[1]["reason"], "error")
        self.assertEqual(wire.read(wire.opened(5, False, "bored"), now=NOW)[1]["error"], "unsupported")
        self.assertEqual(wire.read(wire.test("k", 99), now=NOW)[1]["seconds"], 10)

    def test_stats_drops_bad_entries(self):
        msg = wire.stats(1, {"in:k": [-20, -6], "sideways:k": [1, 1], "out:k": ["a", 1], "in:j": [True, 1]},
                         {"x": {}, "9": {"path": "carrier", "jb_ms": "fast"}})
        read = wire.read(msg, now=NOW)[1]
        self.assertEqual(list(read["levels"]), [("in", "k")])
        self.assertEqual(read["streams"], {9: {"path": "", "jb_ms": None, "loss_pct": None, "rtt_ms": None}})

    def test_stats_howls(self):
        other = protocol.id_text(bytes([9]) * 16)
        self.assertNotIn("howls", wire.stats(1, {}, {})["data"])  # left out when there is none
        msg = wire.stats(1, {}, {}, [{"to": other, "freq_hz": 7100.0, "active": True},
                                     {"to": "not an id", "freq_hz": 7100.0, "active": True},
                                     {"to": other, "freq_hz": "loud", "active": True},
                                     {"to": other, "freq_hz": 99999, "active": False},
                                     {"to": other, "freq_hz": 1000, "active": None}])
        self.assertEqual(wire.read(msg, now=NOW)[1]["howls"], [{"to": other, "freq_hz": 7100.0, "active": True}])
        msg["data"]["howls"] = "many"
        self.assertEqual(wire.read(msg, now=NOW)[1]["howls"], [])
        msg["data"]["howls"] = [{"to": other, "freq_hz": 500, "active": False}] * 20
        self.assertEqual(len(wire.read(msg, now=NOW)[1]["howls"]), wire.MAX_HOWLS)


if __name__ == "__main__":
    unittest.main()
