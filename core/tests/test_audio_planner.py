# SPDX-License-Identifier: GPL-3.0-only
"""The planner, table-driven: on-demand mic, holds, devices that go and come back, sound redirects,
the Windows conflict, unreachable machines, missing virtual mics and the AEC switch."""

import unittest

from core import protocol
from core.audio import model, planner
from core.audio.model import (Device, DeviceRef, FormEntry, MachineAudio, MicRoute, OutputRoute, Route, Stamp,
                              VirtualMic)


def mid(seed):
    return protocol.id_text(bytes([seed]) * 16)


MAC, PC, ROG = mid(1), mid(2), mid(3)
MAC_FEATURES = frozenset({"system_capture", "local_mute_tap", "mic_demand", "virtual_mic", "aec"})
PC_FEATURES = frozenset({"system_capture", "local_mute_endpoint", "mic_demand", "virtual_mic", "aec"})
S = Stamp(10, MAC)


def mac(in_use=False, links=(PC,), output_form="speakers", inputs=None, vmic=True, features=MAC_FEATURES):
    inputs = inputs if inputs is not None else (Device("mic", "MacBook Air Microphone", "input", "builtin", 1, "microphone", True),)
    return MachineAudio(MAC, "mac", "macos", 1, "ok", inputs,
                        (Device("spk", "MacBook Air Speakers", "output", "builtin", 2, output_form, True),),
                        inputs[0].key if inputs else None, "spk", VirtualMic("beamer", vmic, in_use), features,
                        frozenset(links), "0.1.0")


def pc(in_use=False, links=(MAC,), vmic=True, features=PC_FEATURES, outputs=None):
    outputs = outputs if outputs is not None else (
        Device("g733", "Logitech G733", "output", "usb", 2, "headset", True),
        Device("hdmi", "LG HDR 4K", "output", "hdmi", 2, "speakers", False))
    return MachineAudio(PC, "lebig", "windows", 1, "ok",
                        (Device("wave", "Wave:3 Mic In", "input", "usb", 1, "microphone", True),), outputs,
                        "wave", outputs[0].key if outputs else None, VirtualMic("vbcable", vmic, in_use), features,
                        frozenset(links), "0.1.0")


def rog(links=(PC,), in_use=True):
    return MachineAudio(ROG, "rog", "windows", 1, "ok", (), (Device("o", "Out", "output", "usb", 2, "speakers", True),),
                        None, "o", VirtualMic("vbcable", True, in_use), frozenset({"mic_demand"}), frozenset(links), "")


def route(mic=None, device=None, outputs=None, forms=None):
    return Route(MicRoute(mic, device, S), {machine: OutputRoute(to, ref, S) for machine, (to, ref) in (outputs or {}).items()},
                 {name: FormEntry(form, S) for name, form in (forms or {}).items()})


def run(own, machines, value, direct=None, online=None, aec="auto", now=100.0, hold=None):
    machines = {machine.id: machine for machine in machines}
    direct = frozenset(direct if direct is not None else machines[own].links)
    online = frozenset(online if online is not None else model.online(own, direct, machines))
    return planner.plan(own, machines, value, direct, online, aec, now, hold or {})


class MicTests(unittest.TestCase):
    def test_on_demand_table(self):
        mic_ref = DeviceRef("mic", "MacBook Air Microphone")
        cases = [
            # (name, machines, route, expected sends of MAC, loop?, mic state)
            ("off", [mac(), pc(in_use=True)], route(), set(), False, "off"),
            ("nobody records", [mac(), pc()], route(MAC, mic_ref), set(), False, "ready"),
            ("pc records", [mac(), pc(in_use=True)], route(MAC, mic_ref), {("mic", PC)}, False, "streaming"),
            ("mac records itself", [mac(in_use=True), pc()], route(MAC, mic_ref), set(), True, "streaming"),
            ("both record", [mac(in_use=True), pc(in_use=True)], route(MAC, None), {("mic", PC)}, True, "streaming"),
            ("pc without vbcable", [mac(), pc(in_use=True, vmic=False)], route(MAC, mic_ref), set(), False,
             "no_virtual_mic"),
            ("pc without mic_demand", [mac(), pc(features=frozenset({"system_capture"}))], route(MAC, mic_ref),
             {("mic", PC)}, False, "streaming"),
            ("device gone", [mac(inputs=()), pc(in_use=True)], route(MAC, mic_ref), set(), False, "waiting_device"),
            ("mic on the pc", [mac(in_use=True), pc()], route(PC, None), set(), False, "streaming"),
        ]
        for name, machines, value, sends, loop, state in cases:
            with self.subTest(name):
                result = run(MAC, machines, value)
                self.assertEqual(set(result.sends), sends)
                self.assertEqual(result.loop is not None, loop)
                self.assertEqual(result.mic_status.state, state)

    def test_two_consumers_and_the_pc_sees_the_same_status(self):
        machines = [mac(links=(PC,)), pc(in_use=True, links=(MAC, ROG)), rog(links=(PC,))]
        mine = run(MAC, machines, route(PC, None))
        theirs = run(PC, machines, route(PC, None))
        self.assertEqual(mine.mic_status, theirs.mic_status)
        self.assertEqual(set(theirs.sends), {("mic", ROG)})  # MAC does not record: no stream to it
        self.assertEqual(theirs.loop, planner.LoopSpec(None))

    def test_not_direct_is_unreachable(self):
        machines = [mac(links=(PC,)), pc(links=(MAC, ROG)), rog(links=(PC,), in_use=True)]
        result = run(MAC, machines, route(MAC, None))
        self.assertEqual(result.sends, {})
        self.assertEqual(result.mic_status.state, "unreachable")
        self.assertEqual(result.mic_status.params["to"], ROG)

    def test_unique_name_finds_a_moved_device(self):
        moved = (Device("mic-port2", "MacBook Air Microphone", "input", "usb", 1, "microphone", False),)
        result = run(MAC, [mac(inputs=moved), pc(in_use=True)], route(MAC, DeviceRef("mic", "MacBook Air Microphone")))
        self.assertEqual(result.sends[("mic", PC)].source, "mic-port2")

    def test_hold_keeps_demand_and_wakes(self):
        machines = [mac(), pc(in_use=False)]
        held = run(MAC, machines, route(MAC, None), now=100.0, hold={PC: 104.0})
        self.assertEqual(set(held.sends), {("mic", PC)})
        self.assertEqual(held.wake_at, 104.0)
        after = run(MAC, machines, route(MAC, None), now=104.5, hold={PC: 104.0})
        self.assertEqual(after.sends, {})
        self.assertIsNone(after.wake_at)

    def test_offline_mic_machine(self):
        result = run(PC, [mac(), pc(in_use=True, links=())], route(MAC, None), direct=())
        self.assertEqual(result.mic_status.state, "waiting_machine")

    def test_aec_by_form_override_and_mode(self):
        cases = [("speakers", {}, "auto", True), ("headset", {}, "auto", False), ("headphones", {}, "auto", False),
                 ("speakers", {(MAC, "spk"): "headset"}, "auto", False), ("headset", {(MAC, "spk"): "speakers"}, "auto", True),
                 ("speakers", {}, "off", False)]
        for form, forms, mode, aec in cases:
            with self.subTest(form=form, forms=forms, mode=mode):
                result = run(MAC, [mac(output_form=form), pc(in_use=True)], route(MAC, None, forms=forms), aec=mode)
                spec = result.sends[("mic", PC)]
                self.assertEqual(spec.aec, aec)
                self.assertEqual(spec.aec_reference, "spk" if aec else None)
                self.assertEqual(spec.sink, {"virtual_mic": True})

    def test_no_aec_on_the_loop(self):
        result = run(MAC, [mac(in_use=True), pc()], route(MAC, None))
        self.assertEqual(result.loop, planner.LoopSpec(None))


class OutputTests(unittest.TestCase):
    def test_redirect_table(self):
        g733 = DeviceRef("g733", "Logitech G733")
        hdmi = DeviceRef("hdmi", "LG HDR 4K")
        cases = [
            ("local", MAC, [mac(), pc()], {}, set(), "local"),
            ("mac to pc default", MAC, [mac(), pc()], {MAC: (PC, None)}, {("system", PC)}, "streaming"),
            ("mac to pc device", MAC, [mac(), pc()], {MAC: (PC, hdmi)}, {("system", PC)}, "streaming"),
            ("device missing", MAC, [mac(), pc(outputs=(Device("hdmi", "LG HDR 4K", "output", "hdmi", 2, "speakers", True),))],
             {MAC: (PC, g733)}, set(), "waiting_device"),
            ("pc offline", MAC, [mac(links=()), pc(links=())], {MAC: (PC, None)}, set(), "waiting_machine"),
            ("windows conflict", MAC, [mac(), pc()], {MAC: (PC, g733), PC: (MAC, None)}, set(), "conflict"),
            ("other endpoint is fine", MAC, [mac(), pc()], {MAC: (PC, hdmi), PC: (MAC, None)}, {("system", PC)},
             "streaming"),
            ("no system capture", MAC, [mac(features=frozenset({"mic_demand"})), pc()], {MAC: (PC, None)}, set(),
             "error"),
        ]
        for name, own, machines, outputs, sends, state in cases:
            with self.subTest(name):
                result = run(own, machines, route(outputs=outputs))
                self.assertEqual(set(result.sends), sends)
                self.assertEqual(result.output_status[MAC].state, state)

    def test_send_spec_mutes_and_names_the_sink(self):
        result = run(MAC, [mac(), pc()], route(outputs={MAC: (PC, DeviceRef("gone-key", "LG HDR 4K"))}))
        spec = result.sends[("system", PC)]
        self.assertTrue(spec.mute_local)
        self.assertEqual(spec.sink, {"device": DeviceRef("hdmi", "LG HDR 4K")})
        self.assertFalse(spec.aec)
        default = run(MAC, [mac(), pc()], route(outputs={MAC: (PC, None)})).sends[("system", PC)]
        self.assertEqual(default.sink, {"device": None})

    def test_pc_sound_on_the_mac_is_planned_on_the_pc(self):
        result = run(PC, [mac(), pc()], route(outputs={PC: (MAC, None)}))
        self.assertEqual(set(result.sends), {("system", MAC)})
        self.assertEqual(run(MAC, [mac(), pc()], route(outputs={PC: (MAC, None)})).sends, {})

    def test_not_direct(self):
        machines = [mac(links=(PC,)), pc(links=(MAC, ROG)), rog(links=(PC,))]
        result = run(MAC, machines, route(outputs={MAC: (ROG, None)}))
        self.assertEqual(result.output_status[MAC].state, "unreachable")


class SinkTests(unittest.TestCase):
    def test_receiver_checks(self):
        value = route(outputs={PC: (MAC, None)})
        self.assertIsNone(planner.sink_problem(value, pc(), {"virtual_mic": True}))
        self.assertEqual(planner.sink_problem(value, pc(vmic=False), {"virtual_mic": True}), "no_virtual_mic")
        self.assertEqual(planner.sink_problem(value, pc(), {"device": DeviceRef("x", "y")}), "no_device")
        self.assertEqual(planner.sink_problem(value, pc(), {"device": None}), "conflict")
        self.assertIsNone(planner.sink_problem(value, pc(), {"device": DeviceRef("hdmi", "LG HDR 4K")}))
        self.assertIsNone(planner.sink_problem(route(), pc(), {"device": None}))


if __name__ == "__main__":
    unittest.main()
