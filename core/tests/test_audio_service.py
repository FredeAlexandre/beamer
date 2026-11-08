# SPDX-License-Identifier: GPL-3.0-only
"""AudioService with FakeHelper: stream open/close, flow roles, the link fallback and its outbox cap,
helper crash and restart, dev command files, persistence, levels; and link threads never wait."""

import json
import shutil
import tempfile
import threading
import time
import unittest
from pathlib import Path

from core.audio import model, service as service_module, store, wire
from core.audio.fake_helper import FakeHelper
from core.audio.model import DeviceRef
from core.tests.audio_cluster_harness import Net, machine_id


def types(posted, kind=None):
    return [message["type"] if kind is None else message["data"] for _peer, message in posted
            if kind is None or message["type"] == kind]


class PairTests(unittest.TestCase):
    def setUp(self):
        self.net = Net()
        self.addCleanup(self.net.close)
        self.mac = self.net.add(1, "mac", "macos")
        self.pc = self.net.add(2, "lebig", "windows")

    def link(self):
        self.net.link(self.mac, self.pc)
        self.net.settle()

    def test_link_up_order_hello_devices_route(self):
        self.net.link(self.mac, self.pc)
        self.net.pump()
        first = types(self.mac.host.posted)[:3]
        self.assertEqual(first, [wire.HELLO, wire.DEVICES, wire.ROUTE])

    def test_mac_offers_and_pc_answers_on_24823(self):
        self.link()
        offer = self.mac.helper.commands("flow_open")[0]
        answer = self.pc.helper.commands("flow_open")[0]
        self.assertEqual((offer["role"], offer["port"], offer["remote"]), ("offer", 0, None))
        self.assertEqual(offer["host"], self.pc.address)  # bind towards the peer, never INADDR_ANY
        self.assertEqual(self.mac.helper.engine.flows[offer["flow"]]["host"], self.pc.address)
        self.assertEqual((answer["role"], answer["port"]), ("answer", service_module.ANSWER_PORT))
        self.assertEqual(answer["remote"], {"host": self.mac.address, "port": 50001})
        self.assertEqual(answer["key"], offer["key"])
        self.assertEqual(answer["flow"], offer["flow"])
        remote = self.mac.helper.commands("flow_remote")[0]
        self.assertEqual((remote["host"], remote["port"]), (self.pc.address, service_module.ANSWER_PORT))
        self.assertEqual(types(self.pc.host.posted, wire.FLOW), [{"flow": offer["flow"], "phase": "answer",
                                                                    "port": service_module.ANSWER_PORT}])

    def test_loopback_host_means_no_udp(self):
        self.mac.address = "127.0.0.1"
        self.pc.address = None
        self.link()
        self.assertIsNone(self.pc.helper.commands("flow_open")[0]["remote"])
        self.assertIsNone(self.mac.helper.commands("flow_open")[0]["host"])
        self.assertEqual(self.mac.helper.commands("flow_remote"), [])

    def test_mic_on_demand_open_and_close(self):
        self.link()
        self.mac.service.set_mic(self.mac.id, DeviceRef("BuiltInMicrophoneDevice", "MacBook Air Microphone"))
        self.net.settle()
        self.assertEqual(self.mac.helper.commands("send"), [])
        self.assertEqual(self.mac.snapshot().mic_status.state, "ready")
        self.pc.helper.set_in_use(True)
        self.net.settle()
        opened = types(self.mac.host.posted, wire.OPEN)[-1]
        send = self.mac.helper.commands("send")[-1]
        recv = self.pc.helper.commands("recv")[-1]
        self.assertEqual(send["source"], {"device": "BuiltInMicrophoneDevice"})
        self.assertEqual(send["codec"], wire.CODECS["mic"])
        self.assertEqual(send["aec"], {"reference": "BuiltInSpeakerDevice"})  # MacBook speakers: AEC on
        self.assertFalse(send["mute_local"])
        self.assertEqual((recv["stream"], recv["key"], recv["sink"]), (send["stream"], send["key"], {"virtual_mic": True}))
        self.assertEqual(opened["for"], model.stamp_data(self.mac.snapshot().route.mic.stamp))
        self.assertTrue(self.mac.snapshot().aec_active)
        self.assertEqual(self.mac.snapshot().mic_status.state, "streaming")
        # The call ends: the stream stays for the hold, then closes as "demand".
        self.pc.helper.set_in_use(False)
        self.net.settle()
        self.net.clock.advance(4.0)
        self.net.settle()
        self.assertEqual(self.mac.helper.commands("stop"), [])
        self.net.clock.advance(1.5)
        self.net.settle()
        self.assertEqual(self.mac.helper.commands("stop")[-1]["stream"], send["stream"])
        self.assertEqual(types(self.mac.host.posted, wire.CLOSE)[-1], {"stream": send["stream"], "reason": "demand"})
        self.assertEqual(self.pc.helper.commands("stop")[-1]["stream"], send["stream"])
        self.assertEqual(self.mac.snapshot().streams, ())

    def test_local_loop_when_the_mic_machine_records(self):
        self.link()
        self.mac.service.set_mic(self.mac.id, None)
        self.mac.helper.set_in_use(True)
        self.net.settle()
        loop = self.mac.helper.commands("loop")[-1]
        self.assertEqual((loop["source"], loop["sink"]), ({"device": None}, {"virtual_mic": True}))
        self.assertEqual(self.mac.helper.commands("send"), [])

    def test_route_change_closes_and_reopens(self):
        self.link()
        self.pc.service.set_output(self.pc.id, self.mac.id, None)
        self.net.settle()
        first = self.pc.helper.commands("send")[-1]
        self.assertEqual((first["kind"], first["mute_local"], first["source"]), ("system", True, {"system": True}))
        self.mac.service.set_output(self.pc.id, None, None)
        self.net.settle()
        self.assertEqual(types(self.pc.host.posted, wire.CLOSE)[-1], {"stream": first["stream"], "reason": "route"})
        self.assertEqual(self.pc.snapshot().output_status[self.pc.id].state, "local")
        self.assertEqual(self.mac.helper.engine.streams, {})

    def test_refused_open_marks_failed_and_retries_on_change(self):
        self.pc.helper.set_devices(virtual_mic={"kind": "vbcable", "present": True, "in_use": True})
        self.link()
        # The PC lists a virtual mic but its helper refuses the stream.
        self.pc.helper.engine.devices["virtual_mic"]["present"] = False
        self.mac.service.set_mic(self.mac.id, None)
        self.net.settle()
        self.assertEqual(types(self.pc.host.posted, wire.OPENED)[-1]["error"], "no_virtual_mic")
        snap = self.mac.snapshot()
        self.assertEqual(snap.mic_status.state, "error")
        self.assertEqual(snap.mic_status.reason, "no_virtual_mic")
        opens = len(types(self.mac.host.posted, wire.OPEN))
        self.net.round()
        self.net.settle()
        self.assertEqual(len(types(self.mac.host.posted, wire.OPEN)), opens)  # no retry loop
        self.pc.helper.set_devices(virtual_mic={"kind": "vbcable", "present": True, "in_use": True},
                                   inputs=[])  # the PC's list changes: retry
        self.net.settle()
        self.assertEqual(len(types(self.mac.host.posted, wire.OPEN)), opens + 1)
        self.assertEqual(self.mac.snapshot().mic_status.state, "streaming")

    def test_mic_waiting_for_permission_opens_once_granted(self):
        self.link()
        self.mac.helper.engine.mic_error = "permission"
        self.mac.service.set_mic(self.mac.id, None)
        self.pc.helper.set_in_use(True)
        self.net.settle()
        snap = self.mac.snapshot()
        self.assertEqual((snap.mic_status.state, snap.mic_status.reason), ("error", "permission"))
        sends = len(self.mac.helper.commands("send"))
        self.net.round()
        self.net.settle()
        self.assertEqual(len(self.mac.helper.commands("send")), sends)  # no retry loop while waiting
        self.mac.helper.engine.mic_error = None
        self.mac.helper.emit({"ev": "permission", "mic": "undetermined", "system_audio": "unknown"})
        self.net.settle()
        self.assertEqual(len(self.mac.helper.commands("send")), sends)  # nothing answered yet
        self.mac.helper.emit({"ev": "permission", "mic": "granted", "system_audio": "unknown"})
        self.net.settle()
        self.assertEqual(len(self.mac.helper.commands("send")), sends + 1)
        self.assertEqual(self.mac.snapshot().mic_status.state, "streaming")

    def test_system_sound_waiting_for_its_prompt_opens_once_answered(self):
        self.link()
        self.mac.helper.engine.system_error = "permission"
        self.mac.helper.emit({"ev": "permission", "mic": "granted", "system_audio": "undetermined"})
        self.mac.service.set_output(self.mac.id, self.pc.id, None)
        self.net.settle()
        self.assertEqual(self.mac.snapshot().output_status[self.mac.id].reason, "permission")
        sends = len(self.mac.helper.commands("send"))
        self.mac.helper.engine.system_error = None
        self.mac.helper.emit({"ev": "permission", "mic": "granted", "system_audio": "unknown"})
        self.net.settle()
        self.assertEqual(len(self.mac.helper.commands("send")), sends + 1)
        self.assertEqual(self.mac.snapshot().output_status[self.mac.id].state, "streaming")

    def test_receiver_refuses_its_muted_endpoint(self):
        self.link()
        self.pc.service.set_output(self.pc.id, self.mac.id, None)
        self.mac.service.set_output(self.mac.id, self.pc.id, None)  # into the G733, the PC's default
        self.net.settle()
        self.assertEqual(self.mac.snapshot().output_status[self.mac.id].state, "conflict")
        self.assertEqual([s.kind for s in self.mac.snapshot().streams], ["system"])  # only the PC's sound
        self.assertEqual(self.mac.snapshot().streams[0].source, self.pc.id)

    def test_link_fallback_posts_frames_and_caps_the_outbox(self):
        self.link()
        flow = self.mac.helper.commands("flow_open")[0]["flow"]
        self.mac.helper.emit({"ev": "link_out", "flow": flow, "p": ["AAAA", "BBBB"]})
        self.net.pump()
        self.assertEqual(types(self.mac.host.posted, wire.FRAME)[-1], {"flow": flow, "p": ["AAAA", "BBBB"]})
        self.net.settle()
        self.assertEqual(self.pc.helper.commands("link_in")[-1], {"cmd": "link_in", "flow": flow, "p": ["AAAA", "BBBB"]})
        self.mac.host.depth = service_module.OUTBOX_LIMIT + 1
        before = len(types(self.mac.host.posted, wire.FRAME))
        self.mac.helper.emit({"ev": "link_out", "flow": flow, "p": ["CCCC"]})
        self.net.pump()
        self.assertEqual(len(types(self.mac.host.posted, wire.FRAME)), before)
        self.assertEqual(self.mac.service.link_out_dropped, 1)
        # A frame for a flow that is not the current one is ignored.
        self.mac.service.link_message(self.pc.id, wire.frame("00000000", ["AAAA"]))
        links_in = len(self.mac.helper.commands("link_in"))
        self.net.pump()
        self.assertEqual(len(self.mac.helper.commands("link_in")), links_in)

    def test_flow_state_gives_the_stream_path(self):
        self.link()
        self.mac.service.set_mic(self.mac.id, None)
        self.pc.helper.set_in_use(True)
        self.net.settle()
        flow = self.mac.helper.commands("flow_open")[0]["flow"]
        self.mac.helper.emit({"ev": "flow_state", "flow": flow, "state": "link", "rtt_ms": 7.5})
        self.net.pump()
        view = self.mac.snapshot().streams[0]
        self.assertEqual((view.path, view.rtt_ms), ("link", 7.5))

    def test_helper_crash_closes_streams_and_restart_reopens(self):
        self.link()
        self.mac.service.set_mic(self.mac.id, None)
        self.pc.helper.set_in_use(True)
        self.net.settle()
        stream = self.mac.helper.commands("send")[-1]["stream"]
        with self.assertLogs("beamer.audio", "WARNING") as logs:
            self.mac.helper.crash(restart=False)
            self.net.pump()
            self.assertEqual(self.mac.snapshot().helper, "starting")
            self.assertEqual(types(self.mac.host.posted, wire.CLOSE)[-1], {"stream": stream, "reason": "helper"})
            hello = types(self.mac.host.posted, wire.HELLO)[-1]
            self.assertEqual(hello["helper"], "starting")
            self.net.settle()
            self.assertEqual(self.pc.helper.engine.streams, {})
            self.assertEqual(self.mac.snapshot().machines[self.mac.id].status, "starting")
            self.mac.helper.engine.restored = 2
            self.mac.helper.ready()
            self.net.settle()
        self.assertTrue(any("restored 2" in line for line in logs.output))
        self.assertEqual(self.mac.snapshot().helper, "ok")
        self.assertEqual([(s.kind, s.state) for s in self.mac.snapshot().streams], [("mic", "running")])
        self.assertNotEqual(self.mac.helper.commands("send")[-1]["stream"], stream)
        self.assertEqual(len(self.mac.helper.commands("flow_open")), 2)

    def test_answerer_helper_restart_asks_for_a_flow(self):
        self.link()
        self.pc.helper.crash(restart=True)
        self.net.settle()
        self.assertIn({"phase": "want"}, types(self.pc.host.posted, wire.FLOW))
        self.assertEqual(len(self.mac.helper.commands("flow_open")), 2)
        self.assertEqual(len(self.pc.helper.commands("flow_open")), 2)
        self.assertEqual(self.pc.helper.commands("flow_open")[-1]["flow"], self.mac.helper.commands("flow_open")[-1]["flow"])

    def test_helper_failed_and_retry(self):
        self.link()
        self.mac.helper.fail()
        self.net.settle()
        self.assertEqual(self.mac.snapshot().helper, "failed")
        self.assertEqual(self.pc.snapshot().machines[self.mac.id].status, "failed")
        self.mac.service.retry_helper()
        self.net.settle()
        self.assertEqual(self.mac.snapshot().helper, "ok")

    def test_link_down_stops_everything_with_that_peer(self):
        self.link()
        self.pc.service.set_output(self.pc.id, self.mac.id, None)
        self.net.settle()
        self.net.unlink(self.mac, self.pc)
        self.net.pump()
        self.assertEqual(self.pc.helper.engine.streams, {})
        self.assertEqual(self.pc.helper.engine.flows, {})
        self.assertEqual(self.pc.snapshot().machines[self.pc.id].links, frozenset())

    def test_watch_levels_and_stats(self):
        self.link()
        seen = []
        self.mac.service.subscribe_levels(seen.append)
        self.mac.service.watch(True)
        self.net.settle()
        self.assertEqual(self.mac.helper.commands("meter")[-1]["on"], True)
        self.assertEqual(self.pc.helper.commands("meter")[-1]["on"], True)
        self.pc.helper.emit(self.pc.helper.engine.levels())
        self.mac.helper.emit(self.mac.helper.engine.levels())
        self.net.clock.advance(0.3)
        self.net.settle()
        self.net.clock.advance(0.3)
        self.net.settle()
        self.assertTrue(seen)
        keys = set(seen[-1])
        self.assertIn(f"{self.mac.id}/in/BuiltInMicrophoneDevice", keys)
        self.assertIn(f"{self.pc.id}/out/{{out-g733}}", keys)
        self.assertEqual(seen[-1][f"{self.pc.id}/in/{{mic-wave3}}"], (-20.0, -6.0))
        # Coalesced: a burst of level events gives at most one call per 50 ms.
        calls = len(seen)
        for _ in range(10):
            self.mac.helper.emit(self.mac.helper.engine.levels())
            self.net.pump()
        self.assertLessEqual(len(seen) - calls, 1)
        self.mac.service.watch(False)
        self.net.settle()
        self.assertFalse(self.pc.helper.commands("meter")[-1]["on"])
        self.assertIn({"on": False}, types(self.mac.host.posted, wire.WATCH))

    def test_test_input_on_a_peer(self):
        self.link()
        self.mac.service.test_input(self.pc.id, "{mic-wave3}")
        self.net.settle()
        self.assertEqual(self.pc.helper.commands("test_input")[-1], {"cmd": "test_input", "device": "{mic-wave3}",
                                                                       "seconds": 10})

    def test_malformed_and_capless_messages_are_ignored(self):
        self.link()
        before = self.mac.snapshot()
        self.mac.service.link_message(self.pc.id, {"type": wire.ROUTE, "data": {"mic": "x"}})
        self.mac.service.link_message(self.pc.id, {"type": "audio_unknown", "data": {}})
        self.mac.service.link_message(self.pc.id, "garbage")
        stranger = machine_id(9)
        self.mac.service.link_up(stranger, frozenset({"settings"}))
        bad_route = wire.route(model.Route(model.MicRoute(stranger, None, model.Stamp(10 ** 10, stranger))))
        self.mac.service.link_message(stranger, bad_route)
        self.net.pump()
        self.assertEqual(self.mac.snapshot().route, before.route)
        self.assertNotIn(stranger, [peer for peer, _message in self.mac.host.posted])

    def test_own_list_rev_only_grows(self):
        self.link()
        revs = [self.mac.snapshot().machines[self.mac.id].rev]
        for in_use in (True, False, True):
            self.mac.helper.set_in_use(in_use)
            self.net.settle()
            revs.append(self.mac.snapshot().machines[self.mac.id].rev)
        self.assertEqual(revs, sorted(set(revs)))
        self.assertEqual(self.pc.snapshot().machines[self.mac.id].rev, revs[-1])

    def test_form_override_reaches_the_owners_list(self):
        self.link()
        self.pc.service.set_form(self.mac.id, "BuiltInSpeakerDevice", "headset")
        self.net.settle()
        own = self.mac.snapshot().machines[self.mac.id]
        self.assertEqual(own.outputs[0].form, "headset")
        self.assertEqual(self.pc.snapshot().machines[self.mac.id].outputs[0].form, "headset")


class RestartTests(unittest.TestCase):
    """A route entry change restarts only the streams whose spec it changes (§6.4 rule 3)."""

    def setUp(self):
        self.net = Net()
        self.addCleanup(self.net.close)
        self.mac = self.net.add(1, "mac", "macos")
        self.pc = self.net.add(2, "lebig", "windows")
        self.net.link(self.mac, self.pc)
        self.mac.helper.set_in_use(True)
        self.net.settle()

    def sends(self, kind):
        return [command for command in self.pc.helper.commands("send") if command["kind"] == kind]

    def test_pc_mic_and_output_in_one_change_open_each_stream_once(self):
        # WP10 saw the PC's mic open twice when both were set together. Set from either machine,
        # delivered together or one by one, each stream opens once.
        wave = DeviceRef("{mic-wave3}", "Wave:3 Mic In")
        for setter, together in ((self.mac, True), (self.pc, False), (self.mac, False)):
            with self.subTest(setter=setter.name, together=together):
                setter.service.set_mic(None, None)
                setter.service.set_output(self.pc.id, None, None)
                self.net.settle()
                mics, systems = len(self.sends("mic")), len(self.sends("system"))
                stops = len(self.pc.helper.commands("stop"))
                setter.service.set_mic(self.pc.id, wave)
                if not together:
                    self.net.settle()
                setter.service.set_output(self.pc.id, self.mac.id, None)
                self.net.settle()
                self.assertEqual(len(self.sends("mic")) - mics, 1)
                self.assertEqual(len(self.sends("system")) - systems, 1)
                self.assertEqual(len(self.pc.helper.commands("stop")), stops)
                self.assertEqual({s.kind for s in self.pc.snapshot().streams if s.state == "running"},
                                 {"mic", "system"})

    def test_unrelated_entries_never_restart_a_running_stream(self):
        wave = DeviceRef("{mic-wave3}", "Wave:3 Mic In")
        self.mac.service.set_mic(self.pc.id, wave)
        self.mac.service.set_output(self.pc.id, self.mac.id, None)
        self.net.settle()
        sent = len(self.pc.helper.commands("send"))
        running = {command["stream"] for command in self.pc.helper.commands("send")}
        changes = [
            lambda: self.mac.service.set_output(self.mac.id, self.pc.id, DeviceRef("{out-hdmi}", "LG HDR 4K")),
            lambda: self.mac.service.set_output(self.mac.id, None, None),
            lambda: self.pc.service.set_form(self.pc.id, "{out-hdmi}", "headset"),   # not the default output
            lambda: self.mac.service.set_form(self.mac.id, "BuiltInSpeakerDevice", "headset"),
            lambda: self.mac.service.set_mic(self.pc.id, wave),                      # same mic, newer stamp
            lambda: self.pc.service.set_output(self.pc.id, self.mac.id, None),       # same output, newer stamp
            lambda: self.mac.service.set_aec("off"),
        ]
        for change in changes:
            change()
            self.net.settle()
        self.assertEqual(len(self.pc.helper.commands("send")), sent)
        self.assertFalse(running & {command["stream"] for command in self.pc.helper.commands("stop")})
        self.assertEqual(types(self.pc.host.posted, wire.CLOSE), [])
        self.assertEqual(len(self.mac.helper.commands("recv")), 2)  # the PC's mic and sound, opened once

    def test_a_late_dev_test_source_is_what_restarts_the_mic(self):
        # The WP10 double open: the harness's PC `test_source` reached the PC after the route (scp +
        # ssh + the 1 s poll), so the mic opened on the real device, then again on the test source.
        # The dev status names the source in force, for the harness to wait on.
        Path(self.pc.state_dir, "audio-dev.enabled").write_text("{}")
        self.pc.restart()
        self.net.unlink(self.mac, self.pc)
        self.net.link(self.mac, self.pc)
        self.net.settle()
        self.mac.service.set_mic(self.pc.id, DeviceRef("{mic-wave3}", "Wave:3 Mic In"))
        self.net.settle()
        self.assertEqual(self.sends("mic")[-1]["source"], {"device": "{mic-wave3}"})
        Path(self.pc.state_dir, service_module.DEV_CMD).write_text(json.dumps({"test_source": {"kind": "mic", "signal": "B"}}))
        self.net.clock.advance(1.1)
        self.net.settle()
        self.assertEqual(self.sends("mic")[-1]["source"], {"test": "B"})
        status = json.loads(Path(self.pc.state_dir, service_module.DEV_STATUS).read_text())
        self.assertEqual(status["test_source"], "B")


class VmicExclusiveTests(unittest.TestCase):
    """The PC's local "low-latency virtual microphone" (§8.4 `local.vmic_exclusive`): the helper's
    command line, a restart when it changes, persisted, never sent to another machine."""

    def setUp(self):
        self.net = Net()
        self.addCleanup(self.net.close)
        self.mac = self.net.add(1, "mac", "macos")
        self.pc = self.net.add(2, "lebig", "windows")
        self.net.link(self.mac, self.pc)
        self.net.settle()

    def saved_local(self, node):
        self.net.clock.advance(0.6)
        self.net.pump()
        return json.loads((Path(node.state_dir) / store.FILE_NAME).read_text())["local"]

    def test_turning_it_on_restarts_the_helper_with_the_flag(self):
        self.assertNotIn("--vmic-exclusive", self.pc.helper.cmd)
        self.assertNotIn("vmic_exclusive", self.pc.snapshot().machines[self.pc.id].features)
        self.assertFalse(self.pc.snapshot().vmic_exclusive)
        # The Mac's mic plays into the PC's CABLE Input while it changes.
        self.mac.service.set_mic(self.mac.id, None)
        self.pc.helper.set_in_use(True)
        self.net.settle()
        recv = self.pc.helper.commands("recv")[-1]["stream"]
        starts = self.pc.helper.starts
        self.pc.service.set_vmic_exclusive(True)
        self.net.settle()
        self.assertEqual(self.pc.helper.starts, starts + 1)
        self.assertEqual(self.pc.helper.cmd[-1], "--vmic-exclusive")
        self.assertIn("--control", self.pc.helper.cmd)
        snap = self.pc.snapshot()
        self.assertTrue(snap.vmic_exclusive)
        self.assertEqual(snap.helper, "ok")
        self.assertIn("vmic_exclusive", snap.machines[self.pc.id].features)
        self.assertIn({"stream": recv, "reason": "helper"}, types(self.pc.host.posted, wire.CLOSE))
        # The stream comes back on the new helper (drops for about a second on a real one).
        self.assertNotEqual(self.pc.helper.commands("recv")[-1]["stream"], recv)
        self.assertEqual([(v.kind, v.state) for v in self.pc.snapshot().streams], [("mic", "running")])
        self.assertEqual(self.saved_local(self.pc), {"aec": "auto", "vmic_exclusive": True})
        # The same value again changes nothing.
        self.pc.service.set_vmic_exclusive(True)
        self.net.settle()
        self.assertEqual(self.pc.helper.starts, starts + 1)

    def test_kept_across_a_restart_and_turned_off(self):
        self.pc.service.set_vmic_exclusive(True)
        self.net.settle()
        self.saved_local(self.pc)
        self.pc.restart()
        self.assertIn("--vmic-exclusive", self.pc.helper.cmd)
        self.assertTrue(self.pc.snapshot().vmic_exclusive)
        self.pc.service.set_vmic_exclusive(False)
        self.net.settle()
        self.assertNotIn("--vmic-exclusive", self.pc.helper.cmd)
        self.assertEqual(self.pc.helper.starts, 2)
        self.assertNotIn("vmic_exclusive", self.pc.snapshot().machines[self.pc.id].features)
        self.assertEqual(self.saved_local(self.pc), {"aec": "auto", "vmic_exclusive": False})

    def test_local_only_and_windows_only(self):
        self.pc.service.set_vmic_exclusive(True)
        self.mac.service.set_vmic_exclusive(True)
        self.net.settle()
        # Never on the wire as a setting (the PC's helper features name it, as they name `aec`).
        def keys(value):
            if isinstance(value, dict):
                return set(value) | set().union(*(keys(item) for item in value.values()))
            if isinstance(value, list):
                return set().union(*(keys(item) for item in value))
            return set()
        for _peer, message in self.pc.host.posted:
            self.assertNotIn("vmic_exclusive", keys(message))
        self.assertTrue(self.mac.snapshot().vmic_exclusive)        # the Mac's own (meaningless) value
        self.assertEqual(self.mac.helper.starts, 1)                 # no restart, no flag on a Mac
        self.assertNotIn("--vmic-exclusive", self.mac.helper.cmd)
        self.pc.service.set_vmic_exclusive(False)
        self.net.settle()
        self.assertTrue(self.mac.snapshot().vmic_exclusive)

    def test_a_replaced_helpers_late_events_are_dropped(self):
        old = self.pc.service._helper_gen
        self.pc.service.set_vmic_exclusive(True)
        self.net.settle()
        self.pc.service._queue.put(("helper", {"ev": "_state", "state": "failed", "why": "exited"}, old))
        self.net.settle()
        self.assertEqual(self.pc.snapshot().helper, "ok")

    def test_dev_command(self):
        Path(self.pc.state_dir, "audio-dev.enabled").write_text("{}")
        self.pc.restart()
        Path(self.pc.state_dir, service_module.DEV_CMD).write_text(json.dumps({"set_vmic_exclusive": True}))
        self.net.clock.advance(1.1)
        self.net.settle()
        self.assertIn("--vmic-exclusive", self.pc.helper.cmd)
        self.assertIn("--dev", self.pc.helper.cmd)
        self.net.clock.advance(1.1)
        self.net.settle()
        status = json.loads(Path(self.pc.state_dir, service_module.DEV_STATUS).read_text())
        self.assertTrue(status["snapshot"]["vmic_exclusive"])


class HowlTests(unittest.TestCase):
    """The howl guard's `howl` event (§3.3): in the sender's snapshot, and in a watching peer's."""

    def setUp(self):
        self.net = Net()
        self.addCleanup(self.net.close)
        self.mac = self.net.add(1, "mac", "macos")
        self.pc = self.net.add(2, "lebig", "windows")
        self.net.link(self.mac, self.pc)
        self.net.settle()
        self.pc.service.set_output(self.pc.id, self.mac.id, None)
        self.net.settle()
        self.stream = self.pc.helper.commands("send")[-1]["stream"]

    def howl(self, active, stream=None):
        self.pc.helper.emit({"ev": "howl", "stream": stream or self.stream, "active": active, "freq_hz": 7100.0,
                             "duck_db": -20.0 if active else 0.0})
        self.net.settle()

    def test_howl_shows_on_the_sender_and_a_watching_receiver(self):
        self.mac.service.watch(True)
        self.net.settle()
        self.howl(True)
        expected = (model.HowlView(self.pc.id, self.mac.id, 7100.0, True),)
        self.assertEqual(self.pc.snapshot().howls, expected)
        self.net.clock.advance(0.3)
        self.net.settle()
        self.net.clock.advance(0.3)
        self.net.settle()
        self.assertEqual(self.mac.snapshot().howls, expected)
        # Released: the notice stays for the hold, then goes, on both.
        self.howl(False)
        self.assertEqual(self.pc.snapshot().howls, (model.HowlView(self.pc.id, self.mac.id, 7100.0, False),))
        self.net.clock.advance(service_module.HOWL_NOTICE_HOLD - 1)
        self.net.settle()
        self.assertEqual(len(self.pc.snapshot().howls), 1)
        self.net.clock.advance(1.5)
        self.net.settle()
        self.net.clock.advance(0.3)
        self.net.settle()
        self.assertEqual(self.pc.snapshot().howls, ())
        self.assertEqual(self.mac.snapshot().howls, ())
        # Without watching, the receiver drops what it held.
        self.howl(True)
        self.mac.service.watch(False)
        self.net.settle()
        self.assertEqual(self.mac.snapshot().howls, ())

    def test_stream_closed_while_ducked_ends_the_howl(self):
        self.howl(True)
        self.pc.service.set_output(self.pc.id, None, None)
        self.net.settle()
        self.assertEqual(self.pc.snapshot().howls, (model.HowlView(self.pc.id, self.mac.id, 7100.0, False),))

    def test_unknown_streams_and_mic_streams_are_ignored(self):
        self.howl(True, stream=12345)
        self.assertEqual(self.pc.snapshot().howls, ())
        self.pc.service.set_mic(self.pc.id, None)
        self.mac.helper.set_in_use(True)
        self.net.settle()
        mic = [c for c in self.pc.helper.commands("send") if c["kind"] == "mic"][-1]["stream"]
        self.howl(True, stream=mic)
        self.assertEqual(self.pc.snapshot().howls, ())


class StoreTests(unittest.TestCase):
    def setUp(self):
        self.net = Net()
        self.addCleanup(self.net.close)

    def test_saved_after_the_debounce_and_loaded_on_start(self):
        a = self.net.add(1, "mac", "macos")
        b = self.net.add(2, "lebig", "windows")
        self.net.link(a, b)
        a.service.set_mic(b.id, DeviceRef("{mic-wave3}", "Wave:3 Mic In"))
        a.service.set_aec("off")
        self.net.settle()
        path = Path(a.state_dir) / store.FILE_NAME
        self.net.clock.advance(0.6)
        self.net.pump()
        data = json.loads(path.read_text())
        self.assertEqual(data["version"], 1)
        self.assertEqual(data["route"]["mic"]["machine"], b.id)
        self.assertEqual(data["local"], {"aec": "off", "vmic_exclusive": False})
        self.assertIn(b.id, data["machines"])
        self.assertGreater(data["own_rev"], 0)
        loaded = store.load(a.state_dir)
        self.assertEqual(loaded.route, a.snapshot().route)
        self.assertEqual(loaded.aec, "off")

    def test_bad_file_loads_empty(self):
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder)
        (Path(folder) / store.FILE_NAME).write_text("{not json")
        with self.assertLogs("beamer.audio", "WARNING"):
            self.assertEqual(store.load(folder), store.Stored())
        (Path(folder) / store.FILE_NAME).write_text(json.dumps({"version": 1, "route": {"mic": 3}, "own_rev": -4}))
        self.assertEqual(store.load(folder), store.Stored())


class DevCommandTests(unittest.TestCase):
    def setUp(self):
        self.net = Net()
        self.addCleanup(self.net.close)

    def test_dev_files(self):
        a = self.net.add(1, "mac", "macos")
        b = self.net.add(2, "lebig", "windows")
        Path(a.state_dir, "audio-dev.enabled").write_text(json.dumps({"helper": ["/opt/beamer-audio-null"]}))
        a.restart()
        self.assertIn("--dev", a.helper.cmd)
        self.assertEqual(a.helper.cmd[0], "/opt/beamer-audio-null")
        self.net.link(a, b)
        self.net.settle()
        command = Path(a.state_dir, service_module.DEV_CMD)
        command.write_text(json.dumps([{"set_mic": {"machine": a.id, "device": None}},
                                       {"test_source": {"kind": "mic", "signal": "A"}}]))
        b.helper.set_in_use(True)
        self.net.clock.advance(1.1)
        self.net.settle()
        self.assertFalse(command.exists())
        self.assertEqual(a.snapshot().route.mic.machine, a.id)
        self.assertEqual(a.helper.commands("send")[-1]["source"], {"test": "A"})
        self.assertTrue(service_module._test_signal_ok("B:0.2"))
        self.assertFalse(service_module._test_signal_ok("B:2"))
        self.assertFalse(service_module._test_signal_ok("C"))
        status = json.loads(Path(a.state_dir, service_module.DEV_STATUS).read_text())
        self.assertEqual(status["snapshot"]["own"], a.id)
        command.write_text(json.dumps({"set_output": {"machine": a.id, "to": b.id, "device": None}}))
        self.net.clock.advance(1.1)
        self.net.settle()
        self.net.clock.advance(1.1)
        self.net.settle()
        status = json.loads(Path(a.state_dir, service_module.DEV_STATUS).read_text())
        self.assertEqual(status["snapshot"]["output_status"][a.id]["state"], "streaming")
        self.assertEqual({s["kind"] for s in status["snapshot"]["streams"]}, {"mic", "system"})
        command.write_text(json.dumps({"helper_dev": {"drop_udp": True}}))
        self.net.clock.advance(1.1)
        self.net.settle()
        self.assertEqual(a.helper.commands("dev")[-1], {"cmd": "dev", "drop_udp": True})
        events = [json.loads(line) for line in
                  Path(a.state_dir, service_module.DEV_EVENTS).read_text().splitlines()]
        names = {event["ev"] for event in events}
        self.assertIn("ready", names)
        self.assertIn("_link_up", names)
        self.assertNotIn("levels", names)
        self.assertTrue(all("mono" in event and "wall" in event for event in events))

    def test_without_the_switch_nothing_is_read(self):
        a = self.net.add(1, "mac", "macos")
        command = Path(a.state_dir, service_module.DEV_CMD)
        command.write_text(json.dumps({"set_mic": {"machine": a.id, "device": None}}))
        self.net.clock.advance(2)
        self.net.pump()
        self.assertTrue(command.exists())
        self.assertNotIn("--dev", a.helper.cmd)


class SilentHost:
    """A host whose `post` asserts it runs on the service's worker thread, and can be made to stall."""

    def __init__(self):
        self.service = None
        self.stall = threading.Event()
        self.stall.set()
        self.posted = []
        self.wrong_thread = []

    def own_id(self):
        return machine_id(1)

    def own_name(self):
        return "mac"

    def platform(self):
        return "macos"

    def post(self, peer, message):
        if threading.current_thread() is not self.service._thread:
            self.wrong_thread.append(threading.current_thread().name)
        self.stall.wait(5)
        self.posted.append((peer, message))
        return True

    def outbox_depth(self, peer):
        return 0

    def peer_host(self, peer):
        return "192.168.1.2"

    def peer_name(self, peer):
        return "lebig"


class ThreadTests(unittest.TestCase):
    def test_link_threads_only_enqueue(self):
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder)
        host = SilentHost()
        helper = FakeHelper(platform="macos")

        def factory(cmd, deliver):
            helper.bind(deliver)
            return helper
        service = service_module.AudioService(host, folder, ["beamer-audio"], helper_factory=factory)
        host.service = service
        service.start()
        self.addCleanup(service.stop)
        peer = machine_id(2)
        host.stall.clear()          # every post on the worker now blocks
        slowest = 0.0
        hello = wire.hello("windows", "ok", True)
        devices = {"type": wire.DEVICES, "data": {"machine": peer, "rev": 1, "name": "lebig", "platform": "windows",
                                                 "status": "ok", "inputs": [], "outputs": [], "links": []}}
        errors = []

        def link_thread():
            nonlocal slowest
            try:
                for index in range(200):
                    started = time.perf_counter()
                    if index == 0:
                        service.link_up(peer, frozenset({"audio"}))
                    service.link_message(peer, hello if index % 2 else devices)
                    service.snapshot()
                    slowest = max(slowest, time.perf_counter() - started)
            except Exception as error:  # pragma: no cover - reported below
                errors.append(error)
        threads = [threading.Thread(target=link_thread) for _ in range(4)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(5)
        self.assertEqual(errors, [])
        self.assertLess(slowest, 0.05)
        service.set_mic(machine_id(1), None)  # UI calls do not wait either
        host.stall.set()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not any(m["type"] == wire.ROUTE for _p, m in host.posted[3:]):
            time.sleep(0.01)
        self.assertEqual(host.wrong_thread, [])
        self.assertTrue(host.posted)
        service.stop()
        self.assertFalse(service._thread.is_alive())


if __name__ == "__main__":
    unittest.main()
